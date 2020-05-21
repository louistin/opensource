/*
 * Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "log.h"

// 可读写事件, 对 read/write 事件的封装
/* due to limitations in the epoll interface, we need to keep track of
 * all file descriptors outself.
 */
struct evepoll {
  struct event *evread;
  struct event *evwrite;
};

// epoll 事件模型核心数据结构, 对数据进行封装, 所有事件的 read/write 都由此操作
// 封装 epoll 相关变量
struct epollop {
  struct evepoll *fds;  // 记录所有的可读写事件
  int nfds;   // fd 个数
  struct epoll_event *events;   // 存储事件的套接字和事件类型数组
  int nevents;  // 数组大小
  int epfd;
};

static void *epoll_init	(struct event_base *);
static int epoll_add	(void *, struct event *);
static int epoll_del	(void *, struct event *);
static int epoll_dispatch	(struct event_base *, void *, struct timeval *);
static void epoll_dealloc	(struct event_base *, void *);

const struct eventop epollops = {
  "epoll",
  epoll_init,
  epoll_add,
  epoll_del,
  epoll_dispatch,
  epoll_dealloc,
  1 /* need reinit */
};

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

/* On Linux kernels at least up to 2.6.24.4, epoll can't handle timeout
 * values bigger than (LONG_MAX - 999ULL)/HZ.  HZ in the wild can be
 * as big as 1000, and LONG_MAX can be as small as (1<<31)-1, so the
 * largest number of msec we can support here is 2147482.  Let's
 * round that down by 47 seconds.
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)

#define INITIAL_NFILES 32
#define INITIAL_NEVENTS 32
#define MAX_NEVENTS 4096

// 初始化创建 epfd
static void *epoll_init(struct event_base *base) {
  int epfd;
  struct epollop *epollop;

  // 环境变量设置 EVENT_NOEPOLL 时, 需要禁用 EPOLL
  /* Disable epollueue when this environment variable is set */
  if (evutil_getenv("EVENT_NOEPOLL")) {
    return (NULL);
  }

  // 调用 epoll 系统调用, 创建 epfd 句柄, 告诉内核这个监听的数目为32000, 其中包含 epfd
  /* Initalize the kernel queue */
  if ((epfd = epoll_create(32000)) == -1) {
    if (errno != ENOSYS) {
      event_warn("epoll_create");
    }

    return (NULL);
  }

  // TODO: 暂时这里还不是很明白
  FD_CLOSEONEXEC(epfd);

  if (!(epollop = calloc(1, sizeof(struct epollop)))) {
    return (NULL);
  }

  epollop->epfd = epfd;

  // 预先分配 INITIAL_NEVENTS 32 大小的数组, 主要存储事件的套接字和事件类型
  /* Initalize fields */
  epollop->events = malloc(INITIAL_NEVENTS * sizeof(struct epoll_event));
  if (epollop->events == NULL) {
    free(epollop);
    return (NULL);
  }
  epollop->nevents = INITIAL_NEVENTS;

  // 记录所有可读写事件, 预分配 INITIAL_NFILES 32 大小的数组
  epollop->fds = calloc(INITIAL_NFILES, sizeof(struct evepoll));
  if (epollop->fds == NULL) {
    free(epollop->events);
    free(epollop);
    return (NULL);
  }
  epollop->nfds = INITIAL_NFILES;

  // 信号事件模块初始化
  evsignal_init(base);

  return (epollop);
}

static int epoll_recalc(struct event_base *base, void *arg, int max) {
  struct epollop *epollop = arg;

  if (max >= epollop->nfds) {
    struct evepoll *fds;
    int nfds;

    nfds = epollop->nfds;
    while (nfds <= max) {
      nfds <<= 1;
    }

    fds = realloc(epollop->fds, nfds * sizeof(struct evepoll));
    if (fds == NULL) {
      event_warn("realloc");
      return (-1);
    }
    epollop->fds = fds;
    memset(fds + epollop->nfds, 0, (nfds - epollop->nfds) * sizeof(struct evepoll));
    epollop->nfds = nfds;
  }

  return (0);
}

// 监听事件发生, 并将就绪事件添加到就绪事件队列, 然后返回
static int epoll_dispatch(struct event_base *base, void *arg, struct timeval *tv) {
  struct epollop *epollop = arg;
  struct epoll_event *events = epollop->events;
  struct evepoll *evep;
  int i, res, timeout = -1;

  if (tv != NULL) {
    // 转换为毫秒, +999 是为了防止舍位
    timeout = tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
  }

  // 设置最大超时时间
  if (timeout > MAX_EPOLL_TIMEOUT_MSEC) {
    /* Linux kernels can wait forever if the timeout is too big;
     * see comment on MAX_EPOLL_TIMEOUT_MSEC. */
    timeout = MAX_EPOLL_TIMEOUT_MSEC;
  }

  // MAJOR: 监听事件发生
  res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);
  if (res == -1) {
    if (errno != EINTR) { // EINTR 系统中断信号
      event_warn("epoll_wait");
      return (-1);
    }

    // 由于信号事件发生中断, 处理信号事件, 需要将信号事件加入就绪队列
    // MAJOR: 处理信号事件
    evsignal_process(base);
    return (0);

  } else if (base->sig.evsignal_caught) {
    // 有信号事件发生, 处理信号事件, 需要将信号事件加入就绪队列
    // MAJOR: 处理信号事件
    evsignal_process(base);
  }

  event_debug(("%s: epoll_wait reports %d", __func__, res));

  // MAJOR: 处理就绪事件
  for (i = 0; i < res; i++) {
    int what = events[i].events;  // 就绪类型
    struct event *evread = NULL, *evwrite = NULL;
    int fd = events[i].data.fd; // event 的文件描述符

    if (fd < 0 || fd >= epollop->nfds) {
      continue;
    }

    evep = &epollop->fds[fd]; // 取出 fd 对应的读写事件

    // TODO: 这一段需要后面再细究下
    // EPOLLHUP 文件描述符被挂断
    // EPOLLERR 文件描述符发生错误
    // EPOLLIN 文件描述符可读
    // EPOLLOUT 文件描述符可写
    if (what & (EPOLLHUP|EPOLLERR)) {
      evread = evep->evread;
      evwrite = evep->evwrite;
    } else {
      if (what & EPOLLIN) {
        evread = evep->evread;
      }

      if (what & EPOLLOUT) {
        evwrite = evep->evwrite;
      }
    }

    if (!(evread||evwrite)) {
      continue;
    }

    // MAJOR: 添加 event 到就绪事件队列中
    if (evread != NULL) {
      event_active(evread, EV_READ, 1);
    }

    if (evwrite != NULL) {
      event_active(evwrite, EV_WRITE, 1);
    }
  }

  // 注册事件全部就绪, 将 events 数组扩大为原来的两倍
  if (res == epollop->nevents && epollop->nevents < MAX_NEVENTS) {
    /* We used all of the event space this time.  We should
       be ready for more events next time. */
    int new_nevents = epollop->nevents * 2;
    struct epoll_event *new_events;

    new_events = realloc(epollop->events, new_nevents * sizeof(struct epoll_event));
    if (new_events) {
      epollop->events = new_events;
      epollop->nevents = new_nevents;
    }
  }

  return (0);
}

// event_add() 中, I/O 事件和信号事件需要将 event 注册到 I/O 多路复用要监听的事件队列中
static int epoll_add(void *arg, struct event *ev) {
  struct epollop *epollop = arg;
  struct epoll_event epev = {0, {0}};
  struct evepoll *evep;
  int fd, op, events;

  // MAJOR: 信号事件添加, 需要执行信号回调函数注册等
  if (ev->ev_events & EV_SIGNAL) {
    return (evsignal_add(ev));
  }

  fd = ev->ev_fd;
  if (fd >= epollop->nfds) {
    /* Extent the file descriptor array as necessary */
    if (epoll_recalc(ev->ev_base, epollop, fd) == -1) {
      return (-1);
    }
  }

  evep = &epollop->fds[fd];
  op = EPOLL_CTL_ADD;
  events = 0;
  if (evep->evread != NULL) {
    events |= EPOLLIN;
    op = EPOLL_CTL_MOD;
  }
  if (evep->evwrite != NULL) {
    events |= EPOLLOUT;
    op = EPOLL_CTL_MOD;
  }

  if (ev->ev_events & EV_READ) {
    events |= EPOLLIN;
  }

  if (ev->ev_events & EV_WRITE) {
    events |= EPOLLOUT;
  }

  epev.data.fd = fd;
  epev.events = events;
  // MAJOR: 添加/修改文件描述符到 epoll 监听列表
  if (epoll_ctl(epollop->epfd, op, ev->ev_fd, &epev) == -1) {
    return (-1);
  }

  /* Update events responsible */
  if (ev->ev_events & EV_READ) {
    evep->evread = ev;
  }

  if (ev->ev_events & EV_WRITE) {
    evep->evwrite = ev;
  }

  return (0);
}

static int epoll_del(void *arg, struct event *ev) {
  struct epollop *epollop = arg;
  struct epoll_event epev = {0, {0}};
  struct evepoll *evep;
  int fd, events, op;
  int needwritedelete = 1, needreaddelete = 1;

  if (ev->ev_events & EV_SIGNAL)
    return (evsignal_del(ev));

  fd = ev->ev_fd;
  if (fd >= epollop->nfds)
    return (0);
  evep = &epollop->fds[fd];

  op = EPOLL_CTL_DEL;
  events = 0;

  if (ev->ev_events & EV_READ)
    events |= EPOLLIN;
  if (ev->ev_events & EV_WRITE)
    events |= EPOLLOUT;

  if ((events & (EPOLLIN|EPOLLOUT)) != (EPOLLIN|EPOLLOUT)) {
    if ((events & EPOLLIN) && evep->evwrite != NULL) {
      needwritedelete = 0;
      events = EPOLLOUT;
      op = EPOLL_CTL_MOD;
    } else if ((events & EPOLLOUT) && evep->evread != NULL) {
      needreaddelete = 0;
      events = EPOLLIN;
      op = EPOLL_CTL_MOD;
    }
  }

  epev.events = events;
  epev.data.fd = fd;

  if (needreaddelete)
    evep->evread = NULL;
  if (needwritedelete)
    evep->evwrite = NULL;

  if (epoll_ctl(epollop->epfd, op, fd, &epev) == -1)
    return (-1);

  return (0);
}

static void epoll_dealloc(struct event_base *base, void *arg) {
  struct epollop *epollop = arg;

  evsignal_dealloc(base);
  if (epollop->fds)
    free(epollop->fds);
  if (epollop->events)
    free(epollop->events);
  if (epollop->epfd >= 0)
    close(epollop->epfd);

  memset(epollop, 0, sizeof(struct epollop));
  free(epollop);
}
