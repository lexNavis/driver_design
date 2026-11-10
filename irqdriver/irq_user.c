#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    int fd;
    char buffer[4096];
    ssize_t bytes_read;

    fd = open("/proc/irqdriver", O_RDONLY);
    if (fd < 0) {
        perror("open /proc/irqdriver");
        return 1;
    }
    
    bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("=== Statistics from /proc/irqdriver ===\n%s", buffer);
    } else {
        printf("No data read from /proc/irqdriver\n");
    }
    close(fd);

    return 0;
}
