
/**
 * File: asgn2.c
 * Date: 19/09/2015
 * Author: Joshua La Pine 
 * Version: 0.1
 *
 * This is a module which serves as a virtual ramdisk which disk size is
 * limited by the amount of memory available and serves as the requirement for
 * COSC440 assignment 2 in 2012.
 *
 * Note: multiple devices and concurrent modules are not supported in this
 *       version.
 */
 
/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/circ_buf.h>
#include "gpio.h"

#define MYDEV_NAME "asgn2"
#define MYIOC_TYPE 'k'
#define BUF_SIZE 1024
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joshua La Pine");
MODULE_DESCRIPTION("COSC440 asgn2");


/**
 * The node structure for the memory page linked list.
 */ 
typedef struct page_node_rec {
  struct list_head list;
  struct page *page;
} page_node;

typedef struct circ_buffer_def{
  u8 buf[BUF_SIZE];
  int head;
  int tail;
} circ_buffer_type;

typedef struct page_queue_def{
  int head_index;
  int tail_index;
  size_t head_offset;
  size_t tail_offset;
} page_queue_type;

typedef struct asgn2_dev_t {
  dev_t dev;            /* the device */
  struct cdev *cdev;   
  struct list_head mem_list; /*pointer to the head of the page list*/ 
  int num_pages;        /* number of memory pages this module currently holds */
  size_t data_size;     /* total data size in this module */
  atomic_t nprocs;      /* number of processes accessing this device */ 
  atomic_t max_nprocs;  /* max number of processes accessing this device */
  struct kmem_cache *cache;      /* cache memory */
  struct class *class;     /* the udev class */
  struct device *device;   /* the udev device node */
} asgn2_dev;

int null_location = -1;
int bottom_half();
DECLARE_TASKLET(producer, bottom_half, 0);
circ_buffer_type circ_buffer;
page_queue_type page_queue;

asgn2_dev asgn2_device;
struct proc_dir_entry *asgn2_proc;        /*Proc entry*/

int asgn2_major = 0;                      /* major number of module */  
int asgn2_minor = 0;                      /* minor number of module */
int asgn2_dev_count = 1;                  /* number of devices */

/**
 * This function frees all memory pages held by the module.
 */
void free_memory_pages(void) {
  page_node *curr, *temp;

  /* loops through the page list and frees each page*/
  list_for_each_entry_safe(curr, temp, &asgn2_device.mem_list, list){
    if(curr->page != NULL){
      __free_page(curr->page);
    }
    list_del(&curr->list);
    kfree(curr);
    printk(KERN_INFO "Freed memory\n");
  }

  /* resets data size and num pages to initial values*/
  asgn2_device.data_size = 0;
  asgn2_device.num_pages = 0;
  
}


/**
 * This function opens the virtual disk, if it is opened in the write-only
 * mode, all memory pages will be freed.
 */
int asgn2_open(struct inode *inode, struct file *filp) {

  /*Prevents number of processes from exceeding the max*/
  if(atomic_read(&asgn2_device.nprocs) >= atomic_read(&asgn2_device.max_nprocs))
    return -EBUSY;

  atomic_inc(&asgn2_device.nprocs);

  /*Frees memory pages when device opened in write only mode*/
  if((filp->f_flags & O_ACCMODE) == O_WRONLY){
    printk(KERN_INFO "Write only");
    free_memory_pages();
  }

  return 0; /* success */
}


/**
 * This function releases the virtual disk, but nothing needs to be done
 * in this case. 
 */
int asgn2_release (struct inode *inode, struct file *filp) {

  /*Decrements number of processes*/
  atomic_dec(&asgn2_device.nprocs);  
  return 0;
}


irqreturn_t dummyport_interrupt(int irq, void*dev_id){

  /*Use a spinlock when writing to and reading from the circular buffer to avoid race conditions
    spinlock because an interrupt is not allowed to sleep*/

  
  static u8 half_byte;
  static int sig_flag = 1;
  
  if(sig_flag == 1){
    half_byte = read_half_byte() << 4;
    sig_flag = 0;
    // printk(KERN_INFO "read sig\n");
  } else {
    half_byte = half_byte | read_half_byte();
    //printk(KERN_INFO "read least sig\n");
    sig_flag = 1;
    //add to circular buffer
    //printk(KERN_INFO "buffer space remaining: %d\n", CIRC_SPACE(circ_buffer.tail, circ_buffer.head, BUF_SIZE));
    if(CIRC_SPACE(circ_buffer.tail, circ_buffer.head, BUF_SIZE) > 0){
      circ_buffer.buf[circ_buffer.tail] = half_byte;
      circ_buffer.tail = (circ_buffer.tail + 1) % BUF_SIZE;
      //call tasklet
      tasklet_schedule(&producer);
    
    } else {
      //printk(KERN_INFO "BUFFER FULL, Head = %d, Tail = %d\n", circ_buffer.head, circ_buffer.tail);
    }
  }
    
  
  

  return IRQ_HANDLED;
}


int bottom_half(){

  int count = CIRC_CNT(circ_buffer.tail, circ_buffer.head, BUF_SIZE);
  // printk(KERN_INFO "count = %d\n", count);
 
  size_t size_written = 0;  /* size written to virtual disk in this function */
  size_t begin_offset = 0;   /* the offset from the beginning of a page to start writing */
  int begin_page_no = page_queue.tail_index;// + (page_queue.tail_offset %PAGE_SIZE);  /* the first page this finction
  //    should start writing to */
  //might be worth changing f_pos to a different identifier
  int f_pos = page_queue.tail_offset;
  int curr_page_no = 0;     /* the current page number */
 
  size_t size_to_copy;      /* keeps track of how much data is left to copy for a given page*/
  page_node *curr;        /*pointer to a page node for use with list for each entry loop*/

  /* Allocates as many pages as necessary to store count bytes*/
  //printk(KERN_INFO "num pages * page size = %d\n", asgn2_device.num_pages * PAGE_SIZE);
  //printk(KERN_INFO "data size + count = %d\n", asgn2_device.data_size + count);
  while(asgn2_device.num_pages * PAGE_SIZE < asgn2_device.data_size + count){
    curr = kmalloc(sizeof(page_node), GFP_KERNEL);
    if(curr){
      curr->page = alloc_page(GFP_KERNEL);
    } else {
      printk(KERN_WARNING "page_node allocation failed\n");
      return -ENOMEM;
    }
    if(curr->page == NULL){
      printk(KERN_WARNING "Page allocation failed\n");
      return -ENOMEM;
    }
    //printk(KERN_INFO "allocated page %d\n", asgn2_device.num_pages);
    list_add_tail(&(curr->list), &asgn2_device.mem_list);
    //printk(KERN_INFO "added page to list%d\n", asgn2_device.num_pages);
    asgn2_device.num_pages++;
  }
 
  /* Loops through each page in the list and writes the appropriate amount to each one*/
  list_for_each_entry(curr, &asgn2_device.mem_list, list){
    //printk(KERN_INFO "current page no = %d\n", curr_page_no);
    if(curr_page_no >= begin_page_no && count > 0){
      //printk(KERN_INFO "page no = %d\n", curr_page_no);
      begin_offset = f_pos % PAGE_SIZE;
      size_to_copy = min((int)count,(int)( PAGE_SIZE - begin_offset));
    
      //maybe put in a spinlock here?
      memcpy(page_address(curr->page) + begin_offset, circ_buffer.buf + circ_buffer.head, size_to_copy);
      circ_buffer.head = (circ_buffer.head + size_to_copy) % BUF_SIZE;
 
      size_written += size_to_copy;
      count -= size_to_copy;
      f_pos += size_to_copy; /* updates f_pos to correctly calculate begin_offset and update file position pointer*/
      size_to_copy = 0;
      begin_offset = f_pos % PAGE_SIZE;
      // printk(KERN_INFO "offset = %d, count = %d\n", begin_offset, count);
    }
    curr_page_no++;
    
  }

  //printk(KERN_INFO "SIZE WRITTEN = %d\n", size_written);
  asgn2_device.data_size += size_written; //((page_queue.tail_index - page_queue.head_index) * PAGE_SIZE)
  if(begin_offset == 0)
  page_queue.tail_index++;
  page_queue.tail_offset = begin_offset;
  //printk(KERN_INFO "data size = %d, tail index = %d, tail offset = %d\n", asgn2_device.data_size, page_queue.tail_index, page_queue.tail_offset);

  return size_written;
     
}


/**
 * This function reads contents of the virtual disk and writes to the user 
 */
ssize_t asgn2_read(struct file *filp, char __user *buf, size_t count,
                   loff_t *f_pos) {
  size_t size_read = 0;     /* size read from virtual disk in this function */
  size_t begin_offset;      /* the offset from the beginning of a page to
                               start reading */
  int begin_page_no = page_queue.head_index; /* the first page which contains
                                             the requested data */
  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_read;    /* size read from the virtual disk in this round */
  size_t size_to_be_read;   /* size to be read in the current round in 
                               while loop */
  size_t size_to_copy;      /* keeps track of size of data to copy for each page*/
  size_t actual_size;       /* variable to track total data that hasn't yet been read*/
  page_node *curr;          /* pointer to a page node for use with list for each entry loop*/
  page_node *temp;
  
  int freed = 0;
  int null_flag = 0;

  if(page_queue.head_index == null_location){
    null_location = -1;
    page_queue.head_index++;
    printk(KERN_INFO "returned due to null");
    return 0;
  }
  
  if(*f_pos > asgn2_device.data_size) return 0; /*Returns if file position is beyond the data size*/

  actual_size = min(count, asgn2_device.data_size - page_queue.head_offset); /*Calculates the acutal size of data to be read*/

  /* loops through page list and reads the appropriate amount from each page*/
  list_for_each_entry_safe(curr, temp, &asgn2_device.mem_list, list){
  
    if(curr_page_no >= begin_page_no){
      begin_offset = page_queue.head_offset;
      size_to_copy = min((int)actual_size,(int)(PAGE_SIZE - begin_offset));
      size_t min =  min((int)actual_size,(int)(PAGE_SIZE - begin_offset));
      struct page *curr_page = page_address(curr->page);
      u8 count;
      unsigned long pointer = curr_page + begin_offset;
      for(count = 0; count + pointer < min + pointer; count++){
        if(*((u8*)(pointer+count)) == '\0'){
          printk(KERN_INFO "Found a null");
          int temp = PAGE_SIZE - (count + begin_offset);
          size_to_copy = ((PAGE_SIZE - begin_offset) - temp);
          null_flag = 1;
          break;
        }
      }

      printk(KERN_INFO "size to copy = %d\n", size_to_copy);
      while(size_to_copy > 0){
        size_to_be_read = copy_to_user(buf + size_read, page_address(curr->page) + begin_offset,
                                       size_to_copy);
        curr_size_read = size_to_copy - size_to_be_read;
        size_to_copy= size_to_be_read;
        actual_size -= curr_size_read;
        size_read += curr_size_read;
        //*f_pos += curr_size_read;
        page_queue.head_offset = (page_queue.head_offset + curr_size_read) % PAGE_SIZE;
        begin_offset = page_queue.head_offset;
      }
      
    }
    if(begin_offset == 0){
      page_queue.head_index++;
      //free previous page
      __free_page(curr->page);
      list_del(&curr->list);
      kfree(curr);
      freed++;
      curr_page_no++;
      asgn2_device.num_pages--;
    }
  }
  
  //recalculate page queue head and tail indices, might need a spinlock here
  page_queue.head_index -= freed;
  page_queue.tail_index -= freed;

  
  if(null_flag == 1){
    null_location = page_queue.head_index;
    null_flag = 0;
  }

  //subtract freed * page_size from data size
  asgn2_device.data_size -= freed * PAGE_SIZE;
  
  return size_read;
}

/**
 * This function writes from the user buffer to the virtual disk of this
 * module
 */
/**
   ssize_t asgn2_write(struct file *filp, const char __user *buf, size_t count,
                    loff_t *f_pos) {
  size_t orig_f_pos = *f_pos; / * the original file position * /
  size_t size_written = 0;  / * size written to virtual disk in this function * /
  size_t begin_offset;   / * the offset from the beginning of a page to start writing * /
  int begin_page_no = *f_pos / PAGE_SIZE;  / * the first page this finction
                                              should start writing to * /

  int curr_page_no = 0;     / * the current page number * /
  size_t curr_size_written; / * size written to virtual disk in this round * /
  size_t size_to_be_written;  / * size to be read in the current round in 
                                 while loop * /
  size_t size_to_copy;      / * keeps track of how much data is left to copy for a given page* /
  page_node *curr;        / *pointer to a page node for use with list for each entry loop* /
 

  / * Allocates as many pages as necessary to store count bytes* /
  while(asgn2_device.num_pages * PAGE_SIZE < orig_f_pos + count){
    curr = kmalloc(sizeof(page_node), GFP_KERNEL);
    if(curr){
      curr->page = alloc_page(GFP_KERNEL);
    } else {
      printk(KERN_WARNING "page_node allocation failed\n");
      return -ENOMEM;
    }
    if(curr->page == NULL){
      printk(KERN_WARNING "Page allocation failed\n");
      return -ENOMEM;
    }
    printk(KERN_INFO "allocated page %d\n", asgn2_device.num_pages);
    list_add_tail(&(curr->list), &asgn2_device.mem_list);
    printk(KERN_INFO "added page to list%d\n", asgn2_device.num_pages);
    asgn2_device.num_pages++;
  }

  / * Loops through each page in the list and writes the appropriate amount to each one* /
  list_for_each_entry(curr, &asgn2_device.mem_list, list){
    printk(KERN_INFO "current page no = %d\n", curr_page_no);
    if(curr_page_no >= begin_page_no){
      begin_offset = *f_pos % PAGE_SIZE;
      size_to_copy = min((int)count,(int)( PAGE_SIZE - begin_offset));
      while(size_to_copy > 0){

        size_to_be_written = copy_from_user(page_address(curr->page) +begin_offset, buf + size_written,
                                            size_to_copy); / * stores the number of bytes that remain to be written* /
        curr_size_written = size_to_copy - size_to_be_written;
        size_to_copy = size_to_be_written;
        size_written += curr_size_written;
        count -= curr_size_written;
        *f_pos += curr_size_written; / * updates f_pos to correctly calculate begin_offset and update file position pointer* /
        begin_offset = *f_pos % PAGE_SIZE;
      }
    }
    curr_page_no++;
    
    }

  asgn2_device.data_size = max(asgn2_device.data_size,
                               orig_f_pos + size_written);
  return size_written;
}*/

#define SET_NPROC_OP 1
#define TEM_SET_NPROC _IOW(MYIOC_TYPE, SET_NPROC_OP, int) 

/**
 * The ioctl function, which is used to set the maximum allowed number of concurrent processes.
 */
long asgn2_ioctl (struct file *filp, unsigned cmd, unsigned long arg) {
  int nr = _IOC_NR(cmd);
  int new_nprocs;
  int result;

  
  /* checks that the command is for this device*/
  if(_IOC_TYPE(cmd) != MYIOC_TYPE) return -EINVAL;

  /* checks that the given command is the only one that exists*/
  if(nr == SET_NPROC_OP){
    if(!access_ok(VERIFY_READ, arg, sizeof(cmd))){ /* verifies that access is allowed*/
      return -EFAULT;
    } else {
   
      result = __get_user(new_nprocs, (int *)arg); /* gets the data from user space*/
      if(result != 0){
        printk(KERN_INFO "_get_user: Bad Access\n");
        return -EFAULT;
      }

      if(new_nprocs <= 0){
        printk(KERN_INFO "new_nprocs <= 0!\n");
        return -EINVAL;
      }

      atomic_set(&asgn2_device.max_nprocs, new_nprocs); /* sets the new number of max processes*/
      printk(KERN_INFO "max_nprocs now = %d\n", new_nprocs);
      return 0;
    }
  }
  
  return -ENOTTY;
}


/**
 * Displays information about current status of the module,
 * which helps debugging. Outputs num_pages, max_nprocs, data_size,
 * and num_procs.
 */
int asgn2_read_procmem(char *buf, char **start, off_t offset, int count,
                       int *eof, void *data) {

  *eof = 1;
  return snprintf(buf, count, "Num Pages = %d\nData Size = %d\n Num Procs = %d\n Max Procs = %d\n",
                  asgn2_device.num_pages, asgn2_device.data_size, atomic_read(&asgn2_device.nprocs), atomic_read(&asgn2_device.max_nprocs));

}

struct file_operations asgn2_fops = {
  .owner = THIS_MODULE,
  .read = asgn2_read,
  //.write = asgn2_write,
  .unlocked_ioctl = asgn2_ioctl,
  .open = asgn2_open,
  .release = asgn2_release
};


/**
 * Initialise the module and create the master device
 */
int __init asgn2_init_module(void){
  int result;

  /* initialise device struct values*/
  printk(KERN_INFO "asgn_2_init: I am alive\n");
  atomic_set(&asgn2_device.nprocs, 0);
  atomic_set(&asgn2_device.max_nprocs, 1);
  asgn2_device.num_pages = 0;
  asgn2_device.data_size = 0;

  /* dynamically allocates a major and minor number to the device*/
  asgn2_device.dev = MKDEV(asgn2_major, asgn2_minor);
  result = alloc_chrdev_region(&asgn2_device.dev, asgn2_minor, asgn2_dev_count, MYDEV_NAME);
  if(result != 0) {
    printk(KERN_INFO "alloc_chrdev went wrong! result = %d\n", result);
    goto fail_device;
  }
  printk(KERN_INFO "asgn_2_init: still alive after major number allocation\n");

  /* initialises and allocates the character device*/
  asgn2_device.cdev = cdev_alloc();
  cdev_init(asgn2_device.cdev, &asgn2_fops);
  asgn2_device.cdev->owner = THIS_MODULE;
  result = cdev_add(asgn2_device.cdev, asgn2_device.dev, asgn2_dev_count);
  if(result != 0) {
    printk(KERN_INFO "cdev init or add failed\n");
    goto fail_device;
  }
  printk(KERN_INFO "asgn_2_init: still alive after character device initialisation\n");
  INIT_LIST_HEAD(&asgn2_device.mem_list);
  printk(KERN_INFO "asgn_2_init: still alive after init list head\n");

  /* creates a proc entry and adds the read method to it*/
  asgn2_proc = create_proc_entry(MYDEV_NAME, 0, NULL);
  if(!asgn2_proc){
    printk(KERN_INFO "Failed to initialise /proc/%s\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_device;
  }

  asgn2_proc->read_proc = asgn2_read_procmem;

  //Init gpio

  gpio_dummy_init();
   
  //Initialise circular buffer
  circ_buffer.head = 0;
  circ_buffer.tail = 0;

  //initialise page queue struct
  page_queue.head_index = 0;
  page_queue.tail_index = 0;
  page_queue.head_offset = 0;
  page_queue.tail_offset = 0;
  
  asgn2_device.class = class_create(THIS_MODULE, MYDEV_NAME);
  if (IS_ERR(asgn2_device.class)) {
  }

  asgn2_device.device = device_create(asgn2_device.class, NULL, 
                                      asgn2_device.dev, "%s", MYDEV_NAME);
  if (IS_ERR(asgn2_device.device)) {
    printk(KERN_WARNING "%s: can't create udev device\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_device;
  }
  
  printk(KERN_WARNING "set up udev entry\n");
  printk(KERN_WARNING "Hello world from %s\n", MYDEV_NAME);
  return 0;

  /* cleanup code called when any of the initialization steps fail */
  /* I ran out of time to make each of the following steps conditional on their creation
     FIX IT JOSH! */
 fail_device:
  printk(KERN_INFO "asgn_2_init: I died prematurely\n");
  class_destroy(asgn2_device.class);
 
  if(asgn2_proc)
    remove_proc_entry(MYDEV_NAME, NULL);
 
  cdev_del(asgn2_device.cdev);
  unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);
  return result;
}


/**
 * Finalise the module. Deallocates everything in the correct order.
 */
void __exit asgn2_exit_module(void){
  device_destroy(asgn2_device.class, asgn2_device.dev);
  class_destroy(asgn2_device.class);
  printk(KERN_WARNING "cleaned up udev entry\n");
  
  free_memory_pages();
  printk(KERN_INFO"successfully freed pages\n");
  if(asgn2_proc)
  remove_proc_entry(MYDEV_NAME, NULL);

  gpio_dummy_exit();
  cdev_del(asgn2_device.cdev);
  printk(KERN_INFO"successfully deleted device\n");
  unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);
  printk(KERN_INFO"successfully unregistered major/minor numbers\n");
  printk(KERN_WARNING "Good bye from %s\n", MYDEV_NAME);
}


module_init(asgn2_init_module);
module_exit(asgn2_exit_module);


