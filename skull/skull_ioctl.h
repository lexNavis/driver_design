#ifndef SKULL_IOCTL_H
#define SKULL_IOCTL_H
/* Checked linux/Documentation/userspace-api/ioctl/ioctl-number.rst
* for free magic number */
#define SKULL_MAGIC 'J'
/* Flush operation - clears the whole buffer 
* Uses magic number J, and command number 0x00,
* requires no data */
#define SKULL_FLUSH _IO(SKULL_MAGIC, 0x00)
#endif