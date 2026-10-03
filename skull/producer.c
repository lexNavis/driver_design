#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

int min(int a, int b) { return a < b ? a : b; }

int main(int argc, char *argv[])
{
  if (argc < 3)
    return 1;
    
  setbuf(stdout, NULL);
  srand(time(NULL) ^ getpid());

  char dev[32];
  size_t buf_size = 256;
  char buf[buf_size];
  int dev_n = atoi(argv[1]);
  if (dev_n < 0 || dev_n > 2)
  {
    return 1;
  }
  snprintf(dev, sizeof(dev), "/dev/skull%s", argv[1]);
  int dev_fd = open(dev, O_WRONLY);
  if (dev_fd < 0)
  {
    printf("Error opening device %s", dev);
    close(dev_fd);
    return 1;
  }
  int ms_delay = atoi(argv[2]);
  while (1)
  {
    int data_src = rand() % 2;
    if (data_src == 0)
    {
      // Keyboard input emulation
      if (argc < 4)
      {
        printf("No input string in console");
        return 1;
      }
      snprintf(buf, buf_size, "%s", argv[3]);
    }
    else
    {
      FILE *f = fopen("data_files/data.txt", "r");
      if (!f)
        return 1;
      fseek(f, 0, SEEK_END);
      long fsize = ftell(f);
      long offset = rand() % fsize;
      int size = min(fsize - offset, rand() % fsize);
      size = min(size, buf_size - 1);
      fseek(f, offset, SEEK_SET);
      fread(buf, 1, size, f);
      buf[size] = '\0';
      fclose(f);
    }

    printf("Try to write '%s', size = %lu\n", buf, strlen(buf));

    ssize_t written = write(dev_fd, buf, strlen(buf));
    if (written < 0)
    {
      perror("Write failed");
      close(dev_fd);
      return 1;
    }
    else if ((size_t)written != strlen(buf))
    {
      fprintf(stderr, "Warning: Partial write (%zd of %lu bytes)\n", written, strlen(buf));
    }
    usleep(ms_delay * 1000);
  }
  printf("Exiting program...\n");
  close(dev_fd);
  return 0;
}
