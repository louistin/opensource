/*
 * Compile with:
 * cc -I/usr/local/include -o event-test event-test.c -L/usr/local/lib -levent
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <event.h>

static void fifo_read(int fd, short event, void *arg) {
  char buf[255];
  int len;
  struct event *ev = arg;

  /* Reschedule this event */
  event_add(ev, NULL);

  fprintf(stderr, "fifo_read called with fd: %d, event: %d, arg: %p\n",
    fd, event, arg);

  len = read(fd, buf, sizeof(buf) - 1);

  if (len == -1) {
    perror("read");
    return;
  } else if (len == 0) {
    fprintf(stderr, "Connection closed\n");
    return;
  }

  buf[len] = '\0';

  fprintf(stdout, "Read: %s\n", buf);
}

int main (int argc, char **argv) {
  struct event evfifo;

  struct stat st;
  const char *fifo = "event.fifo";
  int socket;

  // 获取 fifo 文件信息, 文件存在且为普通文件, 退出
  if (lstat (fifo, &st) == 0) {
    if ((st.st_mode & S_IFMT) == S_IFREG) {
      errno = EEXIST;
      perror("lstat");
      exit (1);
    }
  }

  // 从文件系统中删除指定文件, 并清除这个文件使用的可用资源
  unlink (fifo);
  // 创建有名管道
  if (mkfifo (fifo, 0600) == -1) {
    perror("mkfifo");
    exit (1);
  }

  // 以非阻塞模式打开有名管道
  /* Linux pipes are broken, we need O_RDWR instead of O_RDONLY */
  socket = open (fifo, O_RDWR | O_NONBLOCK, 0);
  if (socket == -1) {
    perror("open");
    exit (1);
  }

  // 提示向管道写数据
  fprintf(stderr, "Write data to %s.\n", fifo);

  /* Initalize the event library */
  event_init();

  // 设置 socket 可读事件, 回调函数 fifo_read()
  /* Initalize one event */
  event_set(&evfifo, socket, EV_READ, fifo_read, &evfifo);

  // 添加事件到就绪队列, 无超时设置
  /* Add it to the active events, without a timeout */
  event_add(&evfifo, NULL);

  // 事件调度, 分发
  event_dispatch();

  return (0);
}