#ifndef RNG_IOCTL_H
#define RNG_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct rng_stats {
    __u64 bytes_written;   /* всего записано через write */
    __u64 bytes_read;      /* всего прочитано через read */
    __u64 bytes_from_hw;   /* всего получено из RDRAND */
    __u64 mix_count;       /* сколько раз вызывался миксер */
    __u64 total_bits;      /* всего бит смешано с пулом */
    __u64 pool_bytes;      /* сколько сейчас в пуле */
};

#define RNG_IOC_MAGIC 'r'
#define RNG_IOC_RESET  _IO(RNG_IOC_MAGIC, 0)
#define RNG_IOC_STATS  _IOR(RNG_IOC_MAGIC, 1, struct rng_stats)

#endif