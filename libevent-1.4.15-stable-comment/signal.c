/*	$OpenBSD: select.c,v 1.2 2002/06/25 15:50:15 mickey Exp $	*/

/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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

#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/queue.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <assert.h>

#include "event.h"
#include "event-internal.h"
#include "evsignal.h"
#include "evutil.h"
#include "log.h"

struct event_base *evsignal_base = NULL;

static void evsignal_handler(int sig);

#define error_is_eagain(err) ((err) == EAGAIN)

/* Callback for when the signal handler write a byte to our signaling socket */
// socket pair socket 可读事件处理回调函数
static void evsignal_cb(int fd, short what, void *arg) {
  static char signals[1];
  ssize_t n;

  n = recv(fd, signals, sizeof(signals), 0);
  if (n == -1) {
    int err = EVUTIL_SOCKET_ERROR();
    if (! error_is_eagain(err))
      event_err(1, "%s: read", __func__);
  }
}

#ifdef HAVE_SETFD
#define FD_CLOSEONEXEC(x) do { \
        if (fcntl(x, F_SETFD, 1) == -1) \
                event_warn("fcntl(%d, F_SETFD)", x); \
} while (0)
#else
#define FD_CLOSEONEXEC(x)
#endif

int evsignal_init(struct event_base *base) {
  int i;

  // 信号处理程序将写入套接字的一端以唤醒事件循环, 然后事件循环扫描已发送的信号
  /*
   * Our signal handler is going to write to one end of the socket
   * pair to wake up our event loop.  The event loop then scans for
   * signals that got delivered.
   */
  // MAJOR: 创建 socket pair, 使用其将信号事件转化为 I/O 事件处理
  if (evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, base->sig.ev_signal_pair) == -1) {
    event_err(1, "%s: socketpair", __func__);
    return -1;
  }

  // TODO: 信号这一块暂时看不大懂, 先放着
  FD_CLOSEONEXEC(base->sig.ev_signal_pair[0]);
  FD_CLOSEONEXEC(base->sig.ev_signal_pair[1]);
  base->sig.sh_old = NULL;
  base->sig.sh_old_max = 0;
  base->sig.evsignal_caught = 0;
  memset(&base->sig.evsigcaught, 0, sizeof(sig_atomic_t)*NSIG);
  // 初始化信号事件链表, 将所有事件都插入到链表中, NSIG 32
  /* initialize the queues for all events */
  for (i = 0; i < NSIG; ++i) {
    TAILQ_INIT(&base->sig.evsigevents[i]);
  }

  evutil_make_socket_nonblocking(base->sig.ev_signal_pair[0]);
  evutil_make_socket_nonblocking(base->sig.ev_signal_pair[1]);

  // MAJOR: 注册信号 socket pair 的读 socket 的可读事件
  // ev_signal_pair[0] 写入数据时, 读 socket 就会得到通知, 触发读事件
  event_set(&base->sig.ev_signal, base->sig.ev_signal_pair[1],
    EV_READ | EV_PERSIST, evsignal_cb, &base->sig.ev_signal);

  base->sig.ev_signal.ev_base = base;
  base->sig.ev_signal.ev_flags |= EVLIST_INTERNAL;

  return 0;
}

/* Helper: set the signal handler for evsignal to handler in base, so that
 * we can restore the original handler when we clear the current one. */
// 为 evsignal 信号注册对应的操作回调函数
int _evsignal_set_handler(struct event_base *base, int evsignal, void (*handler)(int)) {
#ifdef HAVE_SIGACTION
  struct sigaction sa;
#else
  ev_sighandler_t sh;
#endif
  struct evsignal_info *sig = &base->sig;
  void *p;

  /*
   * resize saved signal handler array up to the highest signal number.
   * a dynamic array is used to keep footprint on the low side.
   */
  // 当传入的人 evsignal 信号值比先前最大的信号值还大时, 需要为 struct sigaction **sh_old
  // 指针数组重新分配空间
  if (evsignal >= sig->sh_old_max) {
    int new_max = evsignal + 1; // 最大值 +1
    event_debug(("%s: evsignal (%d) >= sh_old_max (%d), resizing",
          __func__, evsignal, sig->sh_old_max));
    // 调整 sig->sh_old 内存大小
    p = realloc(sig->sh_old, new_max * sizeof(*sig->sh_old));
    if (p == NULL) {
      event_warn("realloc");
      return (-1);
    }

    // memset 新分配的内存块
    memset((char *)p + sig->sh_old_max * sizeof(*sig->sh_old),
        0, (new_max - sig->sh_old_max) * sizeof(*sig->sh_old));

    sig->sh_old_max = new_max;  // 更新最大信号值
    sig->sh_old = p;
  }

  /* allocate space for previous handler out of dynamic array */
  // 为 evsignal 指针 struct sigaction 结构体分配空间
  sig->sh_old[evsignal] = malloc(sizeof *sig->sh_old[evsignal]);
  if (sig->sh_old[evsignal] == NULL) {
    event_warn("malloc");
    return (-1);
  }

  /* save previous handler and setup new handler */
#ifdef HAVE_SIGACTION
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sa.sa_flags |= SA_RESTART;  // 如果信号中断了进程的某个系统调用, 则系统自动启动该系统调用
  sigfillset(&sa.sa_mask);  // 初始化信号集, 将信号集设置为所有信号的集合

  // evsignal 要捕获的信号类型
  // sa 参数指定新的信号处理方式
  // sig->sh_old[evsignal] 返回先前的信号处理方式
  // MAJOR: 检查/修改指定信号相关联的处理动作
  if (sigaction(evsignal, &sa, sig->sh_old[evsignal]) == -1) {
    event_warn("sigaction");
    free(sig->sh_old[evsignal]);
    sig->sh_old[evsignal] = NULL;
    return (-1);
  }
#else
  if ((sh = signal(evsignal, handler)) == SIG_ERR) {
    event_warn("signal");
    free(sig->sh_old[evsignal]);
    sig->sh_old[evsignal] = NULL;
    return (-1);
  }
  *sig->sh_old[evsignal] = sh;
#endif

  return (0);
}

// 信号事件添加
// event_add() 中, 针对信号事件, 需要执行信号事件添加
int evsignal_add(struct event *ev) {
  int evsignal;
  struct event_base *base = ev->ev_base;
  struct evsignal_info *sig = &ev->ev_base->sig;

  if (ev->ev_events & (EV_READ|EV_WRITE)) {
    event_errx(1, "%s: EV_SIGNAL incompatible use", __func__);
  }

  evsignal = EVENT_SIGNAL(ev);  // 要绑定的信号
  assert(evsignal >= 0 && evsignal < NSIG);

  if (TAILQ_EMPTY(&sig->evsigevents[evsignal])) {
    event_debug(("%s: %p: changing signal handler", __func__, ev));

    // MAJOR: 为信号事件注册对应的操作回调函数
    // 通过 sigaction() 注册信号发生时的回调函数, 当有信号产生时, 首先就会触发这个回调函数
    // 执行 evsignal_caught 标志位设置, 向 socket pair 写入数据等, 其中向可写 soket
    // 写入数据又触发了 epoll_wait() I/O 事件, 在 epoll_dispatch() 中需要处理.
    // 如此一来, 信号事件就转化为了 I/O 事件, 岂不美哉
    if (_evsignal_set_handler(base, evsignal, evsignal_handler) == -1) {
      return (-1);
    }

    /* catch signals if they happen quickly */
    evsignal_base = base;

    if (!sig->ev_signal_added) {
      // MAJOR: 将 ev_signal 加入 events 事件队列, 最终会注册到 epoll 监听列表中
      if (event_add(&sig->ev_signal, NULL)) {
        return (-1);
      }

      sig->ev_signal_added = 1;
    }
  }

  /* multiple events may listen to the same signal */
  TAILQ_INSERT_TAIL(&sig->evsigevents[evsignal], ev, ev_signal_next);

  return (0);
}

int _evsignal_restore_handler(struct event_base *base, int evsignal) {
  int ret = 0;
  struct evsignal_info *sig = &base->sig;
#ifdef HAVE_SIGACTION
  struct sigaction *sh;
#else
  ev_sighandler_t *sh;
#endif

  /* restore previous handler */
  sh = sig->sh_old[evsignal];
  sig->sh_old[evsignal] = NULL;
#ifdef HAVE_SIGACTION
  if (sigaction(evsignal, sh, NULL) == -1) {
    event_warn("sigaction");
    ret = -1;
  }
#else
  if (signal(evsignal, *sh) == SIG_ERR) {
    event_warn("signal");
    ret = -1;
  }
#endif
  free(sh);

  return ret;
}

int evsignal_del(struct event *ev) {
  struct event_base *base = ev->ev_base;
  struct evsignal_info *sig = &base->sig;
  int evsignal = EVENT_SIGNAL(ev);

  assert(evsignal >= 0 && evsignal < NSIG);

  /* multiple events may listen to the same signal */
  TAILQ_REMOVE(&sig->evsigevents[evsignal], ev, ev_signal_next);

  if (!TAILQ_EMPTY(&sig->evsigevents[evsignal])) {
    return (0);
  }

  event_debug(("%s: %p: restoring signal handler", __func__, ev));

  return (_evsignal_restore_handler(ev->ev_base, EVENT_SIGNAL(ev)));
}

// 信号处理回调函数
// evsignal_add() 中调用
static void evsignal_handler(int sig) {
  int save_errno = errno;

  if (evsignal_base == NULL) {
    event_warn(
      "%s: received signal %d, but have no base configured",
      __func__, sig);
    return;
  }

  evsignal_base->sig.evsigcaught[sig]++;  // sig 信号触发数 +1
  // epoll_dispatch() 中根据此标志来处理信号事件, 将信号事件加入就绪队列
  evsignal_base->sig.evsignal_caught = 1; // 标志设置为触发状态

#ifndef HAVE_SIGACTION
  signal(sig, evsignal_handler);
#endif

  /* Wake up our notification mechanism */
  // MAJOR: 向 socket pair 可写端写入数据, 触发事件 epoll 可读端可读事件, 将信号事件转化为 I/O 事件
  send(evsignal_base->sig.ev_signal_pair[0], "a", 1, 0);
  errno = save_errno;
}

// epoll_wait() 被信号中断时, 处理信号事件
// 从信号事件队列取出事件, 并将其加入就绪队列
void evsignal_process(struct event_base *base) {
  struct evsignal_info *sig = &base->sig;
  struct event *ev, *next_ev;
  sig_atomic_t ncalls;
  int i;

  base->sig.evsignal_caught = 0;
  // 检查当前的未决信号, 遍历所有 NSIG 32 个信号
  for (i = 1; i < NSIG; ++i) {
    ncalls = sig->evsigcaught[i];
    if (ncalls == 0) {
      continue;
    }

    sig->evsigcaught[i] -= ncalls;

    for (ev = TAILQ_FIRST(&sig->evsigevents[i]); ev != NULL; ev = next_ev) {
      next_ev = TAILQ_NEXT(ev, ev_signal_next);
      // 非持续化事件从信号事件队列中删除
      if (!(ev->ev_events & EV_PERSIST)) {
        event_del(ev);
      }

      // MAJOR: 将信号事件加入就绪队列
      event_active(ev, EV_SIGNAL, ncalls);
    }
  }
}

void evsignal_dealloc(struct event_base *base) {
  int i = 0;
  if (base->sig.ev_signal_added) {
    event_del(&base->sig.ev_signal);
    base->sig.ev_signal_added = 0;
  }
  for (i = 0; i < NSIG; ++i) {
    if (i < base->sig.sh_old_max && base->sig.sh_old[i] != NULL) {
      _evsignal_restore_handler(base, i);
    }
  }

  if (base->sig.ev_signal_pair[0] != -1) {
    EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[0]);
    base->sig.ev_signal_pair[0] = -1;
  }
  if (base->sig.ev_signal_pair[1] != -1) {
    EVUTIL_CLOSESOCKET(base->sig.ev_signal_pair[1]);
    base->sig.ev_signal_pair[1] = -1;
  }
  base->sig.sh_old_max = 0;

  /* per index frees are handled in evsig_del() */
  if (base->sig.sh_old) {
    free(base->sig.sh_old);
    base->sig.sh_old = NULL;
  }
}
