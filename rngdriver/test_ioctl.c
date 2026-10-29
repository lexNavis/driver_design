#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "rng_ioctl.h"

int main(void) {
    int fd = open("/dev/rngdriver", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct rng_stats st;
    if (ioctl(fd, RNG_IOC_STATS, &st) < 0) { perror("ioctl STATS"); return 1; }
    printf("written=%llu read=%llu hw=%llu stir=%llu bits=%llu pool=%llu\n",
           (unsigned long long)st.bytes_written,
           (unsigned long long)st.bytes_read,
           (unsigned long long)st.bytes_from_hw,
           (unsigned long long)st.stir_count,
           (unsigned long long)st.bits_collected,
           (unsigned long long)st.pool_bytes);

    if (ioctl(fd, RNG_IOC_RESET, 0) < 0) { perror("ioctl RESET"); return 1; }
    puts("reset ok");

    close(fd);
    return 0;
}
