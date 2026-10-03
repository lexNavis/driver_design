#include <linux/module.h>       /* MODULE_LICENSE, MODULE_AUTHOR, module_init/exit */
#include <linux/moduleparam.h>  /* module_param, MODULE_PARM_DESC */
#include <linux/init.h>         /* __init, __exit */
#include <linux/types.h>        /* dev_t, size_t, uint и другие типы ядра */
#include <linux/fs.h>           /* struct file, struct inode, file_operations, alloc_chrdev_region, MAJOR/MINOR */
#include <linux/kernel.h>       /* container_of, printk, макросы ядра */
#include <linux/slab.h>         /* kmalloc, kfree */
#include <linux/cdev.h>         /* struct cdev, cdev_init, cdev_add, cdev_del */
#include <linux/uaccess.h>      /* copy_to_user, copy_from_user, __user */
#include <linux/mutex.h>        /* struct mutex, mutex_init, mutex_lock, mutex_unlock */
#include <linux/semaphore.h>    /* struct semaphore, sema_init, down/up */
#include <linux/wait.h>         /* wait_queue_head_t, wait_event_interruptible, wake_up_all */
#include <linux/ioctl.h>        /* _IO, _IOR, _IOW, _IOWR */
#include <linux/atomic.h>       /* atomic_t, atomic_set, atomic_read */

#include "skull_ioctl.h"

/* Each stack cell can store char line of different size */
struct skull_data
{
  char *data;
  size_t size;
};

struct skull_dev
{
  dev_t dev;                /* Device number */
  struct skull_data *stack; /* Stack with data cells */
  size_t size;              /* Current stack size */
  size_t max_size;          /* Max stack size */
  struct cdev cdev;         /* Link to symbol device bound with current driver */
  struct mutex stack_lock;  /* Access to stack (WR) */
  struct semaphore full;    /* Describes the non-empty cells */
  struct semaphore empty;   /* Describes the empty cells */
  /* Stores processes (consumers/producers), 
   * which are waiting when the flusher ends his work */
  wait_queue_head_t flush_wq; 
  atomic_t is_flushing;
};

static int skull_open(struct inode *inode, struct file *filp);
static int skull_release(struct inode *inode, struct file *filp);
static ssize_t skull_read(struct file *filp, char __user *buf, size_t size, loff_t *);
static ssize_t skull_write(struct file *filp, const char __user *buf, size_t size, loff_t *);
static long int skull_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

/* Module parameters */
static size_t MAX_SIZE;
static unsigned int DEV_COUNT;

module_param(MAX_SIZE, ulong, S_IRUGO);
module_param(DEV_COUNT, uint, S_IRUGO);

/* Static global variables*/
static struct file_operations skull_fops = {
    .owner = THIS_MODULE,
    .open = skull_open,
    .release = skull_release,
    .read = skull_read,
    .write = skull_write,
    .unlocked_ioctl = skull_ioctl};

static struct skull_dev *devs;
static dev_t dev_first;

static int __init skull_init(void)
{
  printk(KERN_INFO "Entering skull_init:\n");
  printk(KERN_INFO "Parameters: Max stack size: %lu; Device count: %u\n", MAX_SIZE, DEV_COUNT);
  int err = 0;

  devs = kmalloc(DEV_COUNT * sizeof(struct skull_dev), GFP_KERNEL);
  if (!devs)
  {
    printk(KERN_ERR "skull_init devs: Failed to allocate memory\n");
    return -ENOMEM;
  }

  /* Get first device number and register a device */
  err = alloc_chrdev_region(&dev_first, 0, DEV_COUNT, "skull");
  if (err)
  {
    printk(KERN_INFO "skull_init: alloc_chrdev_region failed. Error: %d \n", err);
    goto free_chrdev;
  }
  unsigned int i = 0;
  for (i = 0; i < DEV_COUNT; i++)
  {
    printk(KERN_INFO "skull_init Init device: Major = %u; Minor = %d\n", MAJOR(dev_first), i);
    devs[i].dev = MKDEV(MAJOR(dev_first), i);
    devs[i].max_size = MAX_SIZE;
    devs[i].stack = kmalloc(devs[i].max_size * sizeof(struct skull_data), GFP_KERNEL);
    if (!devs[i].stack)
    {
      err = -ENOMEM;
      printk(KERN_ERR "skull_init: kmalloc failed\n");
      goto free_all;
    }
    devs[i].size = 0;
    cdev_init(&devs[i].cdev, &skull_fops);
    mutex_init(&devs[i].stack_lock);
    sema_init(&devs[i].full, 0);
    sema_init(&devs[i].empty, MAX_SIZE);
    init_waitqueue_head(&devs[i].flush_wq);
    atomic_set(&devs[i].is_flushing, 0);

    err = cdev_add(&devs[i].cdev, devs[i].dev, 1);
    if (err)
    {
      printk(KERN_ERR "skull_init cdev_add: Failed to register cdev; Error: %d\n", err);
      goto free_all;
    }
  }

  return err;

free_all: /* Cleans registered cdevs, char devices and memory for skull_dev */
  while (i--)
  {
    kfree(devs[i].stack);
    printk(KERN_INFO "skull_init: Delete device cdev: Major = %u; Minor = %d\n", MAJOR(dev_first), i);
    cdev_del(&devs[i].cdev);
  }
free_chrdev: /* Cleans only char devices and memory for skull_dev */
  printk(KERN_INFO "skull_init: unregister_chrdev_region\n");
  unregister_chrdev_region(dev_first, DEV_COUNT);
  kfree(devs);
  return err;
}

static void __exit skull_exit(void)
{
  printk(KERN_INFO "skull_exit: Removing skull_driver\n");
  for (unsigned int i = 0; i < DEV_COUNT; i++)
  {
    printk(KERN_INFO "skull_exit: Free device %u\n", i);
    for (unsigned int j = 0; j < devs[i].size; j++)
    {
      printk(KERN_INFO "skull_exit: Free stack cell %u\n", j);
      kfree(devs[i].stack[j].data);
    }
    kfree(devs[i].stack);
    devs[i].stack = NULL;
    printk(KERN_INFO "skull_exit: Delete device cdev: Major = %u; Minor = %d\n", MAJOR(dev_first), i);
    cdev_del(&devs[i].cdev);
  }
  printk(KERN_INFO "skull_exit: unregister_chrdev_region\n");
  unregister_chrdev_region(dev_first, DEV_COUNT);
  kfree(devs);
  printk(KERN_INFO "skull_exit: Exiting\n");
}

static int skull_open(struct inode *inode, struct file *filp)
{
  printk(KERN_INFO "skull_open: Open request from %u\n", iminor(inode));
  int err = 0;
  struct skull_dev *dev;
  dev = container_of(inode->i_cdev, struct skull_dev, cdev);
  filp->private_data = dev;
  return err;
}

static int skull_release(struct inode *inode, struct file *filp)
{
  printk(KERN_INFO "skull_release: Release request from %u\n", iminor(inode));
  int err = 0;
  return err;
}

static ssize_t skull_read(struct file *filp, char __user *buf, size_t size, loff_t *)
{
  ssize_t err = 0;
  char *str = NULL;
  size_t str_size = 0;
  struct skull_dev *dev = filp->private_data;
  int dev_idx = dev - devs;
  /* Check is flushing is active  */
  if (atomic_read(&dev->is_flushing))
  {
    /* Wait until flusher ends his work  */
    if (wait_event_interruptible(dev->flush_wq, !atomic_read(&dev->is_flushing)))
    {
      printk(KERN_ERR "skull_read: woke by signal\n");
      return -ERESTARTSYS;
    }
  }
  /* Start of producer - consumer critical section */
  if (down_interruptible(&dev->full))
  {
    err = -ERESTARTSYS;
    printk(KERN_ERR "skull_read: Process woke up by signal. Leaving read request\n");
    goto out;
  }
  /* Start of stack critical section */
  mutex_lock(&dev->stack_lock);
  str = dev->stack[dev->size - 1].data;
  str_size = dev->stack[dev->size - 1].size;
  /* Check if the reader has capability to read */
  if (size < str_size)
  {
    err = -EINVAL;
    printk(KERN_ERR "skull_read: Buffer has less space, than needed. Cancel extracting\n");
    goto leave_without;
  }
  dev->size--;
  printk(KERN_INFO "skull_read (dev[%d]): stack size=%lu, data_size=%lu, data='%.*s'\n",
         dev_idx, dev->size, str_size, (int)str_size, str);
  /* End of stack critical section */
  mutex_unlock(&dev->stack_lock);
  /* End of producer - consumer critical section */
  up(&dev->empty);
  unsigned long bytes_missed = copy_to_user(buf, str, str_size);
  kfree(str);
  /* If copy_to_user fails - we lose the stack cell. Just know that */
  if (bytes_missed > 0)
  {
    err = -EFAULT;
    printk(KERN_INFO "skull_read: copy_to_user failed\n");
    goto out;
  }
  return str_size;

leave_without:
  mutex_unlock(&dev->stack_lock);
  up(&dev->full);
out:
  return err;
}

static ssize_t skull_write(struct file *filp, const char __user *buf, size_t size, loff_t *)
{
  ssize_t err = 0;
  char *str = kmalloc(size, GFP_KERNEL);
  if (!str)
  {
    err = -ENOMEM;
    printk(KERN_ERR "skull_write: kmalloc failed\n");
    goto out;
  }
  unsigned long bytes_missed = copy_from_user(str, buf, size);
  if (bytes_missed > 0)
  {
    err = -EFAULT;
    printk(KERN_ERR "skull_write: copy_from_user failed\n");
    goto out;
  }
  struct skull_dev *dev = filp->private_data;
  int dev_idx = dev - devs;
  /* Check if flusher is active */
  if (atomic_read(&dev->is_flushing))
  {
    /* Wait until flusher ends his work  */
    if (wait_event_interruptible(dev->flush_wq, !atomic_read(&dev->is_flushing)))
    {
      err = -ERESTARTSYS;
      goto out; /* out делает kfree(str) */
    }
  }
  /* Start of producer - consumer critical section */
  if (down_interruptible(&dev->empty))
  {
    printk(KERN_ERR "skull_write: Process woke up by signal. Leaving write request\n");
    err = -ERESTARTSYS;
    goto out;
  }
  /* Start of stack critical section */
  mutex_lock(&dev->stack_lock);
  printk(KERN_INFO "skull_write (dev[%d]) : size=%lu, data='%.*s'\n", dev_idx, size, (int)size, str);
  dev->stack[dev->size].data = str;
  dev->stack[dev->size].size = size;
  dev->size++;
  printk(KERN_INFO "skull_write (dev[%d]): Current stack size = %lu\n", dev_idx, dev->size);
  /* End of stack critical section */
  mutex_unlock(&dev->stack_lock);
  /* End of producer - consumer critical section */
  up(&dev->full);
  return size;

out:
  kfree(str);
  return err;
}

static long int skull_ioctl(
    struct file *filp,
    unsigned int cmd,
    unsigned long arg)
{
  switch (cmd)
  {
  case SKULL_FLUSH:
  {
    struct skull_dev *dev = filp->private_data;
    int dev_idx = dev - devs;
    /* Enter flush mode */
    atomic_set(&dev->is_flushing, 1);
    /* Enter stack crititcal section */
    mutex_lock(&dev->stack_lock);
    /* Check if any cells are still full*/
    while (!down_trylock(&dev->full))
    {
      /* Free memory, allocated in write operation */
      printk("skull_flush (dev[%d]): is_flushing string '%s'", dev_idx, dev->stack[dev->size - 1].data);
      kfree(dev->stack[dev->size - 1].data);
      /* Set safe default values */
      dev->stack[dev->size - 1].data = NULL;
      dev->stack[dev->size - 1].size = 0;
      /* Stack top is moving one cell down */
      (dev->size)--;
      printk("Size now is %ld", dev->size);
      /* Now the cell is truly empty, inc empty sem */
      up(&dev->empty);
    }
    /* Leave stack crititcal section*/
    mutex_unlock(&dev->stack_lock);
    /* After flush has ended, wake up consumers/producers */
    atomic_set(&dev->is_flushing, 0);
    wake_up_all(&dev->flush_wq);
    return 0;
  }
  default:
    return -ENOTTY;
    break;
  }
}

module_init(skull_init);
module_exit(skull_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shein Denis");
MODULE_DESCRIPTION("Skull driver, uses stack to store char data of different size");
