
/**
 * File: asgn2.c
 * Date: 19/09/2015
 * Author: Joshua La Pine 
 * Version: 0.1
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
#include <linux/sched.h>
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

/* Defines circular buffer*/
typedef struct circ_buffer_def{
  u8 buf[BUF_SIZE];
  int head;
  int tail;
} circ_buffer_type;

/* Keeps track of page queue information*/
typedef struct page_queue_def{
  int head_index;
  int tail_index;
  size_t head_offset;
  size_t tail_offset;
  //spinlock_t lock;
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

/*Variables for session separation and waiting*/
atomic_t null_flag;
atomic_t wait_flag;
int null_location = -1;

/* Declaration for tasklet and wait queue*/
void bottom_half(unsigned long t_arg);
DECLARE_TASKLET(producer, bottom_half, 0);
DECLARE_WAIT_QUEUE_HEAD(data_wq);
DECLARE_WAIT_QUEUE_HEAD(process_wq);

/* Declaration for various required structs*/
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
 * This function opens the device, will sleep if already opened by another process.
 * Will return -EACCES when not opened when not opened in read only mode.
 */
int asgn2_open(struct inode *inode, struct file *filp) {

  /*Prevents number of processes from exceeding the max*/
  if(atomic_read(&asgn2_device.nprocs) >= atomic_read(&asgn2_device.max_nprocs))
    printk(KERN_INFO "Exceeded max number of processes, going to sleep\n");
    wait_event_interruptible(process_wq, atomic_read(&asgn2_device.nprocs) == 0);

  atomic_inc(&asgn2_device.nprocs);

  /*Returns -EACCES when device not opened in read only mode*/
  if((filp->f_flags & O_ACCMODE) != O_RDONLY){
    return -EACCES;
  }

  return 0; /* success */
}


/**
 * This function resets null_flag and decrements the number of processes when called.
 * Will wake up any sleeping processes that previously tried to open device
 */
int asgn2_release (struct inode *inode, struct file *filp) {

  /*Decrements number of processes*/
  atomic_set(&null_flag, 0);
  atomic_dec(&asgn2_device.nprocs);
  wake_up_interruptible(&process_wq);
  return 0;
}

/**
 * Interrupt handler that reads half bytes from gpio and assembles them into full bytes.
 * When a full byte is assembled it is copied into a circular buffer and a tasklet is scheduled
 */
irqreturn_t dummyport_interrupt(int irq, void*dev_id){
  
  static u8 half_byte;
  static int sig_flag = 1;
  
  if(sig_flag == 1){
    half_byte = read_half_byte() << 4;
    sig_flag = 0;
  } else {
    half_byte = half_byte | read_half_byte();
    //printk(KERN_INFO "read least sig\n");
    sig_flag = 1;
    //add to circular buffer
    
    if(CIRC_SPACE(circ_buffer.tail, circ_buffer.head, BUF_SIZE) > 0){
      circ_buffer.buf[circ_buffer.tail] = half_byte;
      circ_buffer.tail = (circ_buffer.tail + 1) % BUF_SIZE;
      tasklet_schedule(&producer);
    } else if((char)half_byte == '\0'){ //if its a null terminator then write it
      circ_buffer.buf[circ_buffer.tail] = half_byte;
      
    } else {
      printk(KERN_INFO "BUFFER FULL, Head = %d, Tail = %d\n", circ_buffer.head, circ_buffer.tail);
    }
    
  }
    
  
  

  return IRQ_HANDLED;
}


/**
 * Tasklet code. Copies the contents of the circular buffer into the page queue.
 */
void bottom_half(unsigned long t_arg){

  int count;
 
  size_t size_written = 0;  /* size written to virtual disk in this function */
  size_t begin_offset = 0;   /* the offset from the beginning of a page to start writing */
  int begin_page_no = page_queue.tail_index; /* the first page this function should start writing to */
 
  int curr_offset = page_queue.tail_offset;
  int curr_page_no = 0;     /* the current page number */
 
  size_t size_to_copy;      /* keeps track of how much data is left to copy for a given page*/
  page_node *curr;        /*pointer to a page node for use with list for each entry loop*/

  /* Lock page queue before any page allocation or writing*/
  //spin_lock(&page_queue.lock);
  count = CIRC_CNT(circ_buffer.tail, circ_buffer.head, BUF_SIZE);
  
  /* Allocates as many pages as necessary to store count bytes*/
  while(asgn2_device.num_pages * PAGE_SIZE < asgn2_device.data_size + count){
    curr = kmalloc(sizeof(page_node), GFP_KERNEL);
    if(curr){
      curr->page = alloc_page(GFP_KERNEL);
    } else {
      printk(KERN_WARNING "page_node allocation failed\n");
      return;
    }
    if(curr->page == NULL){
      printk(KERN_WARNING "Page allocation failed\n");
      return;
    }
    list_add_tail(&(curr->list), &asgn2_device.mem_list);
    asgn2_device.num_pages++;
  }
 
  /* Loops through each page in the list and writes the appropriate amount to each one*/
  list_for_each_entry(curr, &asgn2_device.mem_list, list){
    if(curr_page_no >= begin_page_no && count > 0){
      begin_offset = curr_offset % PAGE_SIZE;
      size_to_copy = min((int)count,(int)( PAGE_SIZE - begin_offset));
    
      memcpy(page_address(curr->page) + begin_offset, circ_buffer.buf + circ_buffer.head, size_to_copy);
      circ_buffer.head = (circ_buffer.head + size_to_copy) % BUF_SIZE;
 
      size_written += size_to_copy;
      count -= size_to_copy;
      curr_offset += size_to_copy; /* updates curr_offset to correctly calculate begin_offset*/
      size_to_copy = 0;
      begin_offset = curr_offset % PAGE_SIZE;
      // printk(KERN_INFO "offset = %d, count = %d\n", begin_offset, count);
    }
    curr_page_no++;
    
  }

  asgn2_device.data_size += size_written;

  /* increments page tail if offset is 0*/
  if(begin_offset == 0){
  page_queue.tail_index++;
  }

  /*Sets the tail offset to the last offset calculated in the copying loop above*/
  page_queue.tail_offset = begin_offset;


  //spin_unlock(&page_queue.lock);
  
  //wake up read
  atomic_set(&wait_flag, 0);
  wake_up_interruptible(&data_wq);
     
}


/**
 * This function reads contents of the page queue and writes to the user 
 */
ssize_t asgn2_read(struct file *filp, char __user *buf, size_t count,
                   loff_t *f_pos) {
  size_t size_read = 0;     /* size read from page queue in this function */
  size_t begin_offset = 0;      /* the offset from the beginning of a page to start reading */
  int begin_page_no;  /* the first page which contains the requested data */
  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_read;    /* size read from the page queue in this round */
  size_t size_to_be_read;   /* size to be read in the current round in 
                               while loop */
  size_t size_to_copy;      /* keeps track of size of data to copy for each page*/
  size_t actual_size;       /* variable to track total data that hasn't yet been read*/
  page_node *curr;          /* pointer to a page node for use with list for each entry loop*/
  page_node *temp;          /* Used for safe page freeing*/
  
  int freed = 0; /* The number of freed pages, used to recalculate head and tail indices*/

  /*If the head offset is pointing at a null terminator then increment head and return 0*/
  if((int)page_queue.head_offset == null_location){
    null_location = -1;
    page_queue.head_offset++;
    printk(KERN_INFO "returned due to null\n");
    return 0;
  }
  
  /*If the head and tail index are equal and the head and tail offset are equal then the page queue is considered empty*/
  if(page_queue.head_index == page_queue.tail_index && page_queue.head_offset == page_queue.tail_offset){
    atomic_set(&wait_flag, 1);
  }

  /* Puts read to sleep until wait_flag == 0 and the wake up signal is sent*/
  if(atomic_read(&wait_flag) == 1)
  printk(KERN_INFO "waiting for data\n");
  wait_event_interruptible(data_wq, atomic_read(&wait_flag) == 0);

  //spin_lock(&page_queue.lock);
  begin_page_no = page_queue.head_index;

  /* If a null terminator has been found then return*/
  if(atomic_read(&null_flag) == 1) return 0;

  actual_size = min(count, asgn2_device.data_size - page_queue.head_offset); /*Calculates the acutal size of data to be read*/
  
  /* loops through page list and reads the appropriate amount from each page*/
  list_for_each_entry_safe(curr, temp, &asgn2_device.mem_list, list){

    if(curr_page_no >= begin_page_no){

      size_t min; 
      struct page *curr_page;
      unsigned long pointer;
      size_t byte_count;

      begin_offset = page_queue.head_offset;
      min =  min((int)actual_size,(int)(PAGE_SIZE - begin_offset));
      size_to_copy = min;
      curr_page = page_address(curr->page);

      pointer = (unsigned long)curr_page + begin_offset;
      
      /*Searches the page for a null terminator from page head offset until the end or
        until the number of bytes to be read is reached. Then calculates the number of bytes to copy
        and sets a flag to say that a null terminator has been found*/
      for(byte_count = 0; byte_count + pointer < min + pointer; byte_count++){
        
        if(*((u8*)(pointer+byte_count)) == '\0'){

          int temp = PAGE_SIZE - (byte_count + begin_offset);
          printk(KERN_INFO "Found a null at page position %d\n", begin_offset+byte_count);
          size_to_copy = ((PAGE_SIZE - begin_offset) - temp);
          atomic_set(&null_flag, 1);
          break;
        }
      }

      while(size_to_copy > 0){
        size_to_be_read = copy_to_user(buf + size_read, page_address(curr->page) + begin_offset,
                                       size_to_copy);
        curr_size_read = size_to_copy - size_to_be_read;
        size_to_copy= size_to_be_read;
        actual_size -= curr_size_read;
        size_read += curr_size_read;
   
        page_queue.head_offset = (page_queue.head_offset + curr_size_read) % PAGE_SIZE;
        begin_offset = page_queue.head_offset;
      }
      
    }

    /*If at the start of a page then free the previous page*/
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
    
    if(atomic_read(&null_flag) == 1) break;
  }
  
  /*Recalculate page queue head and tail indices*/
  page_queue.head_index -= freed;
  page_queue.tail_index -= freed;

  /*If a null flag has been set then set the null_location global*/
  if(atomic_read(&null_flag) == 1){
    null_location = page_queue.head_offset;
  }

  //spin_unlock(&page_queue.lock);
  
  asgn2_device.data_size -= freed * PAGE_SIZE;
  
  return size_read;
}

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
  .unlocked_ioctl = asgn2_ioctl,
  .open = asgn2_open,
  .release = asgn2_release
};


/**
 * Initialise the module and create the master device
 */
int __init asgn2_init_module(void){
  int result;
  int failstep = -1;
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
    failstep = 2;
    printk(KERN_INFO "cdev init or add failed\n");
    goto fail_device;
  }
  printk(KERN_INFO "asgn_2_init: still alive after character device initialisation\n");
  INIT_LIST_HEAD(&asgn2_device.mem_list);
  printk(KERN_INFO "asgn_2_init: still alive after init list head\n");

  /* creates a proc entry and adds the read method to it*/
  asgn2_proc = create_proc_entry(MYDEV_NAME, 0, NULL);
  if(!asgn2_proc){
    failstep = 1;
    printk(KERN_INFO "Failed to initialise /proc/%s\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_device;
  }

  asgn2_proc->read_proc = asgn2_read_procmem;

  /*Init gpio*/
  gpio_dummy_init();
   
  /*Initialise circular buffer*/
  circ_buffer.head = 0;
  circ_buffer.tail = 0;
  
  /*Initialise page queue struct*/
  page_queue.head_index = 0;
  page_queue.tail_index = 0;
  page_queue.head_offset = 0;
  page_queue.tail_offset = 0;
  //spin_lock_init(&page_queue.lock);
  
  /*Init wait and null flag*/
  atomic_set(&null_flag, 0);
  atomic_set(&wait_flag, 1);
  
  asgn2_device.class = class_create(THIS_MODULE, MYDEV_NAME);
  if (IS_ERR(asgn2_device.class)) {
  }

  asgn2_device.device = device_create(asgn2_device.class, NULL, 
                                      asgn2_device.dev, "%s", MYDEV_NAME);
  if (IS_ERR(asgn2_device.device)) {
    failstep = 0;
    printk(KERN_WARNING "%s: can't create udev device\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_device;
  }
  
  printk(KERN_WARNING "set up udev entry\n");
  printk(KERN_WARNING "Hello world from %s\n", MYDEV_NAME);
  return 0;

  /* cleanup code called when any of the initialization steps fail */
 fail_device:
  printk(KERN_INFO "asgn_2_init: I died prematurely\n");

  switch(failstep){

  case 0:
    remove_proc_entry(MYDEV_NAME, NULL);
    cdev_del(asgn2_device.cdev);
    unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);
    break;
  case 1:
    cdev_del(asgn2_device.cdev);
    unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);
    break;
  case 2:
    unregister_chrdev_region(asgn2_device.dev, asgn2_dev_count);
    break;
  }
  
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


