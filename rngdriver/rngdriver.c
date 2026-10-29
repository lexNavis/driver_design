#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/fcntl.h>
#include <asm/archrandom.h>
#include <asm/cpufeature.h>
#include "rng_ioctl.h"

#define MAX_POOL 8192
#define HW_CHUNK 4096
#define DEVICE_NAME "rngdriver"

static struct rng_dev
{
  u8 pool[MAX_POOL];
  size_t byte_count;
  u32 mix_count;

  u64 total_written;
  u64 total_read;
  u64 total_hw;
  u64 total_bits;

  struct miscdevice misc;
  spinlock_t pool_lock;
  wait_queue_head_t read_wq;

  bool shutdown;
} rng_dev;

static int rng_open(struct inode *inode, struct file *filp);
static int rng_release(struct inode *inode, struct file *filp);
static ssize_t rng_read(struct file *filp, char __user *buf, size_t size, loff_t *);
static ssize_t rng_write(struct file *filp, const char __user *buf, size_t size, loff_t *);
static long int rng_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
static void rng_mix(struct rng_dev *dev, const u8 *data, size_t size);
static ssize_t rng_hw_fill(struct rng_dev *dev, size_t n);

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = rng_open,
    .release = rng_release,
    .read = rng_read,
    .write = rng_write,
    .unlocked_ioctl = rng_ioctl};

static ssize_t get_rand(u8 *buf, size_t size)
{
  size_t copied = 0;

  if (!boot_cpu_has(X86_FEATURE_RDRAND))
    return -ENODEV;

  while (copied < size)
  {
    unsigned long val;
    size_t chunk;

    if (!rdrand_long(&val))
      return -EIO;

    chunk = min(size - copied, sizeof(val));
    memcpy(buf + copied, &val, chunk);
    copied += chunk;
  }
  return copied;
}

static int __init rng_init(void)
{
  printk("rngdriver mounted\n");
  spin_lock_init(&rng_dev.pool_lock);
  init_waitqueue_head(&rng_dev.read_wq);
  rng_dev.misc = (struct miscdevice){
      .minor = MISC_DYNAMIC_MINOR,
      .name = DEVICE_NAME,
      .fops = &fops};
  misc_register(&rng_dev.misc);
  rng_dev.byte_count = 0;
  rng_dev.mix_count = 0;
  rng_dev.total_written = 0;
  rng_dev.total_read = 0;
  rng_dev.total_hw = 0;
  rng_dev.total_bits = 0;
  rng_dev.shutdown = false;
  return 0;
}

static void __exit rng_exit(void)
{
  rng_dev.shutdown = true;
  wake_up_interruptible(&rng_dev.read_wq);
  misc_deregister(&rng_dev.misc);
  printk("rngdriver unmounted\n");
}

/* Смешивает size байт data с пулом. Вызывается под pool_lock. */
static void rng_mix(struct rng_dev *dev, const u8 *data, size_t size)
{
  u32 seed = 0x9E3779B9u;
  u32 prev, x;
  size_t i;

  for (i = 0; i < size; i++)
  {
    seed ^= data[i];
    seed *= 0x85EBCA6Bu;
    seed ^= (seed >> 13);
  }
  seed ^= dev->mix_count;

  prev = seed;
  for (i = 0; i < MAX_POOL; i++)
  {
    x = (u32)dev->pool[i] | (prev << 8);
    x ^= (x << 13);
    x ^= (x >> 17);
    x ^= (x << 5);
    dev->pool[i] = (u8)(x & 0xFF);
    prev = x;
  }

  for (i = 0; i < size; i++)
  {
    dev->pool[i % MAX_POOL] ^= data[i];
    dev->pool[(i + 1) % MAX_POOL] ^= (u8)(data[i] * 31u + dev->mix_count);
  }

  dev->mix_count++;

  if (dev->byte_count + size > MAX_POOL)
    dev->byte_count = MAX_POOL;
  else
    dev->byte_count += size;
}

/* Добирает n байт из RDRAND и смешивает с пулом. Вызывается из rng_read. */
static ssize_t rng_hw_fill(struct rng_dev *dev, size_t n)
{
  u8 *buf;
  ssize_t got;

  if (n > MAX_POOL)
    n = MAX_POOL;

  buf = kmalloc(n, GFP_KERNEL);
  if (!buf)
    return -ENOMEM;

  got = get_rand(buf, n);
  if (got < 0) {
    kfree(buf);
    return got;
  }

  spin_lock(&dev->pool_lock);
  rng_mix(dev, buf, got);
  dev->total_hw += got;
  dev->total_bits += (u64)got * 8;
  spin_unlock(&dev->pool_lock);

  kfree(buf);
  return got;
}

static int rng_open(struct inode *inode, struct file *filp)
{
  return 0;
}

static int rng_release(struct inode *inode, struct file *filp)
{
  return 0;
}

static ssize_t rng_read(struct file *filp, char __user *buf, size_t size, loff_t *)
{
  if (rng_dev.shutdown)
    return -ENODEV;

  if (rng_dev.byte_count == 0)
    rng_hw_fill(&rng_dev, HW_CHUNK);

  while (rng_dev.byte_count == 0)
  {
    if (filp->f_flags & O_NONBLOCK)
      return -EAGAIN;
    if (wait_event_interruptible(rng_dev.read_wq, rng_dev.byte_count > 0 || rng_dev.shutdown))
      return -ERESTARTSYS;
    if (rng_dev.shutdown)
      return -ENODEV;
  }

  spin_lock(&rng_dev.pool_lock);
  size_t n = min(size, rng_dev.byte_count);
  if (n == 0)
  {
    spin_unlock(&rng_dev.pool_lock);
    return 0;
  }
  /* GFP_ATOMIC: kmalloc под спинлоком не может спать */
  u8 *str = kmalloc(n, GFP_ATOMIC);
  if (!str)
  {
    spin_unlock(&rng_dev.pool_lock);
    return -ENOMEM;
  }
  memcpy(str, rng_dev.pool, n);
  memmove(rng_dev.pool, rng_dev.pool + n, rng_dev.byte_count - n);
  rng_dev.byte_count -= n;
  rng_dev.total_read += n;
  spin_unlock(&rng_dev.pool_lock);

  if (copy_to_user(buf, str, n) != 0)
  {
    kfree(str);
    return -EFAULT;
  }
  kfree(str);
  return n;
}

static ssize_t rng_write(struct file *, const char __user *buf, size_t size, loff_t *)
{
  if (size == 0)
    return 0;
  if (size > MAX_POOL)
    return -EINVAL;

  u8 *str = kmalloc(size, GFP_KERNEL);
  if (!str)
    return -ENOMEM;
  if (copy_from_user(str, buf, size) != 0)
  {
    kfree(str);
    return -EFAULT;
  }

  spin_lock(&rng_dev.pool_lock);
  rng_mix(&rng_dev, str, size);
  rng_dev.total_written += size;
  rng_dev.total_bits += (u64)size * 8;
  spin_unlock(&rng_dev.pool_lock);

  wake_up_interruptible(&rng_dev.read_wq);
  kfree(str);
  return size;
}

static long int rng_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
  struct rng_stats stats;

  switch (cmd)
  {
  case RNG_IOC_RESET:
    spin_lock(&rng_dev.pool_lock);
    rng_dev.byte_count = 0;
    rng_dev.mix_count = 0;
    rng_dev.total_written = 0;
    rng_dev.total_read = 0;
    rng_dev.total_hw = 0;
    rng_dev.total_bits = 0;
    spin_unlock(&rng_dev.pool_lock);
    return 0;

  case RNG_IOC_STATS:
    spin_lock(&rng_dev.pool_lock);
    stats.bytes_written = rng_dev.total_written;
    stats.bytes_read = rng_dev.total_read;
    stats.bytes_from_hw = rng_dev.total_hw;
    stats.mix_count = rng_dev.mix_count;
    stats.total_bits = rng_dev.total_bits;
    stats.pool_bytes = rng_dev.byte_count;
    spin_unlock(&rng_dev.pool_lock);

    if (copy_to_user((struct rng_stats __user *)arg, &stats, sizeof(stats)) != 0)
      return -EFAULT;
    return 0;

  default:
    return -ENOTTY;
  }
}

module_init(rng_init);
module_exit(rng_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shein Denis");
MODULE_DESCRIPTION("Accumulates entropy, returns random bytes");