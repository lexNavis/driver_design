#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include "skull_ioctl.h"

int main(int argc, char *argv[])
{
  if (argc < 3)
    return 1;
  
  setbuf(stdout, NULL);
  int dev_n = atoi(argv[1]);
  if (dev_n < 0 || dev_n > 2)
  {
    return 1;
  }
  char dev[32];
  snprintf(dev, sizeof(dev), "/dev/skull%s", argv[1]);
  int dev_fd = open(dev, O_RDWR);
  if (dev_fd < 0)
  {
    printf("Error opening device %s\n", dev);
    close(dev_fd);
    return 1;
  }
  int ms_delay = atoi(argv[2]);
  while (1) {
    usleep(ms_delay * 1000);
    if (ioctl(dev_fd, SKULL_FLUSH, 0) < 0) {
      perror("SKULL_FLUSH failed");
    }
  }
  printf("Exiting program...\n");
  close(dev_fd);
  return 0;
}