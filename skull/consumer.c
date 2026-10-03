#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

int main(int argc, char *argv[])
{
  if (argc < 3)
    return 1;
    
  setbuf(stdout, NULL);
  srand(time(NULL) ^ getpid());
  size_t sizes[] = {100, 200, 300};
  char dev[32];
  int dev_n = atoi(argv[1]);
  if (dev_n < 0 || dev_n > 2)
  {
    return 1;
  }
  snprintf(dev, sizeof(dev), "/dev/skull%s", argv[1]);

  int dev_fd = open(dev, O_RDONLY);
  if (dev_fd < 0)
  {
    printf("Error opening device %s\n", dev);
    close(dev_fd);
    return 1;
  }
  int ms_delay = atoi(argv[2]);
  while (1)
  {
    size_t buf_size = sizes[rand() % 3];
    char buf[buf_size + 1];
    memset(buf, 0, buf_size + 1);
    printf("Try to read from dev '%s'\n", dev);
    ssize_t bytes_read = read(dev_fd, buf, buf_size);
    if (bytes_read < 0)
    {
      perror("Read failed\n");
    }
    else
    {
      printf("Read data is: %s\n", buf);
    }
    usleep(ms_delay * 1000);
  }
  printf("Exiting program...\n");
  close(dev_fd);
  return 0;
}