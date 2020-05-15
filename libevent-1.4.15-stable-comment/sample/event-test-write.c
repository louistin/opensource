#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#define FIFONAME  "event.fifo"
#define MAXLINE   1024

int main(int argc, char *argv[]) {
  int fd;
  char buf[MAXLINE];
  int nwrite;

  if (mkfifo(FIFONAME, O_CREAT | O_EXCL < 0) && (errno != EEXIST)) {
    printf("create fifo error.\n");
  }

  fd = open(FIFONAME, O_WRONLY | O_NONBLOCK, 0);
  if (fd < 0) {
    perror("open error: ");
  }

  memset(buf, 0, sizeof(buf));
  sprintf(buf, "%s", "hello");

  if ((nwrite = write(fd, buf, MAXLINE)) == -1) {
    perror("write error:");
  } else {
    printf("write %s bytes.\n", buf);
  }

  return 0;
}