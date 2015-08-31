
/**
 * File: asgn1.c
 * Date: 13/03/2011
 * Author: Joshua La Pine 
 * Version: 0.1
 *
 * This is a module which serves as a virtual ramdisk which disk size is
 * limited by the amount of memory available and serves as the requirement for
 * COSC440 assignment 1 in 2012.
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

#define MYDEV_NAME "asgn1"
#define MYIOC_TYPE 'k'

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joshua La Pine");
MODULE_DESCRIPTION("COSC440 asgn1");


/**
 * The node structure for the memory page linked list.
 */ 
typedef struct page_node_rec {
  struct list_head list;
  struct page *page;
} page_node;

typedef struct asgn1_dev_t {
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
} asgn1_dev;

asgn1_dev asgn1_device;
struct proc_dir_entry *asgn1_proc;        /*Proc entry*/

int asgn1_major = 0;                      /* major number of module */  
int asgn1_minor = 0;                      /* minor number of module */
int asgn1_dev_count = 1;                  /* number of devices */

/**
 * This function frees all memory pages held by the module.
 */
void free_memory_pages(void) {
  page_node *curr, *temp;

  /* loops through the page list and frees each page*/
  list_for_each_entry_safe(curr, temp, &asgn1_device.mem_list, list){
    if(curr->page != NULL){
      __free_page(curr->page);
    }
    list_del(&curr->list);
    kfree(curr);
    printk(KERN_INFO "Freed memory");
  }

  /* resets data size and num pages to initial values*/
  asgn1_device.data_size = 0;
  asgn1_device.num_pages = 0;
  
}


/**
 * This function opens the virtual disk, if it is opened in the write-only
 * mode, all memory pages will be freed.
 */
int asgn1_open(struct inode *inode, struct file *filp) {

  /*Prevents number of processes from exceeding the max*/
  if(atomic_read(&asgn1_device.nprocs) >= atomic_read(&asgn1_device.max_nprocs))
    return -EBUSY;

  atomic_inc(&asgn1_device.nprocs);

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
int asgn1_release (struct inode *inode, struct file *filp) {

  /*Decrements number of processes*/
  atomic_dec(&asgn1_device.nprocs);  
  return 0;
}


/**
 * This function reads contents of the virtual disk and writes to the user 
 */
ssize_t asgn1_read(struct file *filp, char __user *buf, size_t count,
                   loff_t *f_pos) {
  size_t size_read = 0;     /* size read from virtual disk in this function */
  size_t begin_offset;      /* the offset from the beginning of a page to
                               start reading */
  int begin_page_no = *f_pos / PAGE_SIZE; /* the first page which contains
                                             the requested data */
  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_read;    /* size read from the virtual disk in this round */
  size_t size_to_be_read;   /* size to be read in the current round in 
                               while loop */
  size_t size_to_copy;      /* keeps track of size of data to copy for each page*/
  size_t actual_size;       /* variable to track total data that hasn't yet been read*/
  page_node *curr;          /* pointer to a page node for use with list for each entry loop*/



  if(*f_pos > asgn1_device.data_size) return 0; /*Returns if file position is beyond the data size*/

  actual_size = min(count, asgn1_device.data_size - (size_t) *f_pos); /*Calculates the acutal size of data to be read*/

  /* loops through page list and reads the appropriate amount from each page*/
  list_for_each_entry(curr, &asgn1_device.mem_list, list){
  
    if(curr_page_no >= begin_page_no){
      begin_offset = *f_pos % PAGE_SIZE;
      size_to_copy = min((int)actual_size,(int)(PAGE_SIZE - begin_offset));
      while(size_to_copy > 0){
        size_to_be_read = copy_to_user(buf + size_read, page_address(curr->page) + begin_offset,
                                       size_to_copy);
        curr_size_read = size_to_copy - size_to_be_read;
        size_to_copy= size_to_be_read;
        actual_size -= curr_size_read;
        size_read += curr_size_read;
        *f_pos += curr_size_read;
        begin_offset = *f_pos % PAGE_SIZE;
      }
    }

    curr_page_no++;
  }

  return size_read;
}



/* Seeks through the file. Changes the file position pointer by a given offset*/
static loff_t asgn1_lseek (struct file *file, loff_t offset, int cmd)
{
  loff_t testpos = 0;

  size_t buffer_size = asgn1_device.num_pages * PAGE_SIZE;

  switch(cmd){
  case SEEK_SET:
    testpos = offset;
    break;

  case SEEK_CUR:
    testpos = file->f_pos + offset;
    break;

  case SEEK_END:
    testpos = buffer_size + offset;
    break;
  }

  if(testpos < 0) testpos = 0; /* sets testpos to 0 so the f_pos doesn't end up negative*/
  if(testpos > buffer_size) testpos = buffer_size; /* sets testpos to buffer_size so f_pos is within the file*/

  file->f_pos = testpos;
  
  printk (KERN_INFO "Seeking to pos=%ld\n", (long)testpos);
  return testpos;
}


/**
 * This function writes from the user buffer to the virtual disk of this
 * module
 */
ssize_t asgn1_write(struct file *filp, const char __user *buf, size_t count,
                    loff_t *f_pos) {
  size_t orig_f_pos = *f_pos;  /* the original file position */
  size_t size_written = 0;  /* size written to virtual disk in this function */
  size_t begin_offset;   /* the offset from the beginning of a page to start writing */
  int begin_page_no = *f_pos / PAGE_SIZE;  /* the first page this finction
                                              should start writing to */

  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_written; /* size written to virtual disk in this round */
  size_t size_to_be_written;  /* size to be read in the current round in 
                                 while loop */
  size_t size_to_copy;      /* keeps track of how much data is left to copy for a given page*/
  page_node *curr;        /* pointer to a page node for use with list for each entry loop*/
 

  /* Allocates as many pages as necessary to store count bytes*/
  while(asgn1_device.num_pages * PAGE_SIZE < orig_f_pos + count){
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
    printk(KERN_INFO "allocated page %d\n", asgn1_device.num_pages);
    list_add_tail(&(curr->list), &asgn1_device.mem_list);
    printk(KERN_INFO "added page to list%d\n", asgn1_device.num_pages);
    asgn1_device.num_pages++;
  }

  /* Loops through each page in the list and writes the appropriate amount to each one*/
  list_for_each_entry(curr, &asgn1_device.mem_list, list){
    printk(KERN_INFO "current page no = %d\n", curr_page_no);
    if(curr_page_no >= begin_page_no){
      begin_offset = *f_pos % PAGE_SIZE;
      size_to_copy = min((int)count,(int)( PAGE_SIZE - begin_offset));
      while(size_to_copy > 0){

        size_to_be_written = copy_from_user(page_address(curr->page) +begin_offset, buf + size_written,
                                            size_to_copy); /* stores the number of bytes that remain to be written*/
        curr_size_written = size_to_copy - size_to_be_written;
        size_to_copy = size_to_be_written;
        size_written += curr_size_written;
        count -= curr_size_written;
        *f_pos += curr_size_written; /* updates f_pos to correctly calculate begin_offset and update file position pointer*/
        begin_offset = *f_pos % PAGE_SIZE;
      }
    }
    curr_page_no++;
    
    }

  asgn1_device.data_size = max(asgn1_device.data_size,
                               orig_f_pos + size_written);
  return size_written;
}

#define SET_NPROC_OP 1
#define TEM_SET_NPROC _IOW(MYIOC_TYPE, SET_NPROC_OP, int) 

/**
 * The ioctl function, which is used to set the maximum allowed number of concurrent processes.
 */
long asgn1_ioctl (struct file *filp, unsigned cmd, unsigned long arg) {
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

      atomic_set(&asgn1_device.max_nprocs, new_nprocs); /* sets the new number of max processes*/
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
int asgn1_read_procmem(char *buf, char **start, off_t offset, int count,
                       int *eof, void *data) {

  *eof = 1;
  return snprintf(buf, count, "Num Pages = %d\nData Size = %d\n Num Procs = %d\n Max Procs = %d\n",
                  asgn1_device.num_pages, asgn1_device.data_size, atomic_read(&asgn1_device.nprocs), atomic_read(&asgn1_device.max_nprocs));

}

/**
 * Maps the virtual ramdisk to a virtual memory area in user space.
 * This allows for quicker access by user space programs as it avoids
 * the need for context switching.
 */
static int asgn1_mmap (struct file *filp, struct vm_area_struct *vma)
{
  unsigned long pfn; /* page frame number*/
  unsigned long offset = vma->vm_pgoff << PAGE_SHIFT; /* num of starting page*/
  unsigned long len = vma->vm_end - vma->vm_start; /* length of virtual memory area*/
  unsigned long ramdisk_size = asgn1_device.num_pages * PAGE_SIZE; /* total ramdisk size*/
  page_node *curr; /* pointer to a page_node for use with list for each entry loop*/
  unsigned long index = 0;

  /* returns if the length of the virutal memory area is more than the size of the ramdisk*/
  if(len > ramdisk_size){
    printk(KERN_WARNING "Virutal memory area too large");
    return -EINVAL;
  }

  /* checks that the offset isn't too large*/
  if(offset > asgn1_device.num_pages){
    printk(KERN_WARNING "Not enough pages in ramdisk\n");
    return -EINVAL;
  }

  /* loops through the page list and remaps the appropriate pages to the correct virtual memory area*/
  list_for_each_entry(curr, &asgn1_device.mem_list, list){
    /* ensures that the mapping starts at the appropriate page and doesn't exceed the vma*/
    if(index >= offset && vma->vm_start+(index*PAGE_SIZE) < vma->vm_end){
    pfn = page_to_pfn(curr->page);
    remap_pfn_range(vma, vma->vm_start+(index*PAGE_SIZE), pfn, PAGE_SIZE, vma->vm_page_prot);
    }
    index++;
  }
  return 0;
}


struct file_operations asgn1_fops = {
  .owner = THIS_MODULE,
  .read = asgn1_read,
  .write = asgn1_write,
  .unlocked_ioctl = asgn1_ioctl,
  .open = asgn1_open,
  .mmap = asgn1_mmap,
  .release = asgn1_release,
  .llseek = asgn1_lseek
};


/**
 * Initialise the module and create the master device
 */
int __init asgn1_init_module(void){
  int result;

  /* initialise device struct values*/
  printk(KERN_INFO "asgn_1_init: I am alive\n");
  atomic_set(&asgn1_device.nprocs, 0);
  atomic_set(&asgn1_device.max_nprocs, 1);
  asgn1_device.num_pages = 0;
  asgn1_device.data_size = 0;

  /* dynamically allocates a major and minor number to the device*/
  asgn1_device.dev = MKDEV(asgn1_major, asgn1_minor);
  result = alloc_chrdev_region(&asgn1_device.dev, asgn1_minor, asgn1_dev_count, MYDEV_NAME);
  if(result != 0) {
    printk(KERN_INFO "alloc_chrdev went wrong! result = %d\n", result);
    goto fail_device;
  }
  printk(KERN_INFO "asgn_1_init: still alive after major number allocation\n");

  /* initialises and allocates the character device*/
  asgn1_device.cdev = cdev_alloc();
  cdev_init(asgn1_device.cdev, &asgn1_fops);
  asgn1_device.cdev->owner = THIS_MODULE;
  result = cdev_add(asgn1_device.cdev, asgn1_device.dev, asgn1_dev_count);
  if(result != 0) {
    printk(KERN_INFO "cdev init or add failed\n");
    goto fail_device;
  }
  printk(KERN_INFO "asgn_1_init: still alive after character device initialisation\n");
  INIT_LIST_HEAD(&asgn1_device.mem_list);
  printk(KERN_INFO "asgn_1_init: still alive after init list head\n");

  /* creates a proc entry and adds the read method to it*/
  asgn1_proc = create_proc_entry(MYDEV_NAME, 0, NULL);
  if(!asgn1_proc){
    printk(KERN_INFO "Failed to initialise /proc/%s\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_device;
  }

  asgn1_proc->read_proc = asgn1_read_procmem;
  
  asgn1_device.class = class_create(THIS_MODULE, MYDEV_NAME);
  if (IS_ERR(asgn1_device.class)) {
  }

  asgn1_device.device = device_create(asgn1_device.class, NULL, 
                                      asgn1_device.dev, "%s", MYDEV_NAME);
  if (IS_ERR(asgn1_device.device)) {
    printk(KERN_WARNING "%s: can't create udev device\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_device;
  }
  
  printk(KERN_WARNING "set up udev entry\n");
  printk(KERN_WARNING "Hello world from %s\n", MYDEV_NAME);
  return 0;

  /* cleanup code called when any of the initialization steps fail */
  /* I ran out of time to make each of the following steps conditional on their creation*/
 fail_device:
  printk(KERN_INFO "asgn_1_init: I died prematurely\n");
  class_destroy(asgn1_device.class);
 
  if(asgn1_proc)
    remove_proc_entry(MYDEV_NAME, NULL);
 
  cdev_del(asgn1_device.cdev);
  unregister_chrdev_region(asgn1_device.dev, asgn1_dev_count);
  return result;
}


/**
 * Finalise the module. Deallocates everything in the correct order.
 */
void __exit asgn1_exit_module(void){
  device_destroy(asgn1_device.class, asgn1_device.dev);
  class_destroy(asgn1_device.class);
  printk(KERN_WARNING "cleaned up udev entry\n");
  
  free_memory_pages();
  printk(KERN_INFO"successfully freed pages\n");
  if(asgn1_proc)
  remove_proc_entry(MYDEV_NAME, NULL);
  cdev_del(asgn1_device.cdev);
  printk(KERN_INFO"successfully deleted device\n");
  unregister_chrdev_region(asgn1_device.dev, asgn1_dev_count);
  printk(KERN_INFO"successfully unregistered major/minor numbers\n");
  printk(KERN_WARNING "Good bye from %s\n", MYDEV_NAME);
}


module_init(asgn1_init_module);
module_exit(asgn1_exit_module);


