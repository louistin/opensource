/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
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
#else
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

#ifdef HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif

// C 语言多态
// 根据当前支持的系统调用进行对应的初始化
// 此处的 epollops 对应上面的 extern const struct eventop epollops;
/* In order of preference */
static const struct eventop *eventops[] = {
#ifdef HAVE_EVENT_PORTS
  &evportops,
#endif
#ifdef HAVE_WORKING_KQUEUE
  &kqops,
#endif
#ifdef HAVE_EPOLL
  &epollops,
#endif
#ifdef HAVE_DEVPOLL
  &devpollops,
#endif
#ifdef HAVE_POLL
  &pollops,
#endif
#ifdef HAVE_SELECT
  &selectops,
#endif
  NULL  // 以 NULL 为数组结尾, 遍历时不需要依靠计算数组大小就可以在结尾处结束遍历
};

/* Global state */
struct event_base *current_base = NULL; // 初始化后全局的 event_base
extern struct event_base *evsignal_base;
static int use_monotonic;   // 是否使用 monotonic 时间

/* Handle signals - This is a deprecated interface */
int (*event_sigcb)(void);		/* Signal callback when gotsig is set */
volatile sig_atomic_t event_gotsig;	/* Set in signal handler */

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

// 设置 use_monotonic = 1
static void detect_monotonic(void) {
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
  struct timespec	ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    use_monotonic = 1;
#endif
}

// 获取 base 时间, 有缓存优先使用缓存事件, 没有则获取当前时间
static int gettime(struct event_base *base, struct timeval *tp) {
  if (base->tv_cache.tv_sec) {
    *tp = base->tv_cache;
    return (0);
  }

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
  if (use_monotonic) {
    struct timespec	ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
      return (-1);

    tp->tv_sec = ts.tv_sec;
    tp->tv_usec = ts.tv_nsec / 1000;
    return (0);
  }
#endif

  return (evutil_gettimeofday(tp, NULL));
}

// 初始化 event_base
// 1. 在堆上分配 event_base 内存空间
// 2. 初始化 event_base 成员
// 3. 选择 I/O 多路复用机制
// 4. 初始化事件优先级队列
// 5. 设置 event_base 为全局 current_base
struct event_base * event_init(void) {
  struct event_base *base = event_base_new();

  // 创建 base 后将其赋给全局变量 current_base
  if (base != NULL)
    current_base = base;

  return (base);
}

struct event_base * event_base_new(void) {
  int i;
  struct event_base *base;

  // 在堆上分配内存存储 base, 所有字段初始化为 0
  if ((base = calloc(1, sizeof(struct event_base))) == NULL)
    event_err(1, "%s: calloc", __func__);

  event_sigcb = NULL;
  event_gotsig = 0;

  // 检测系统是否支持 monotonic 时钟类型(monotonic 时间自系统开机后一直单调递增, 不计休眠)
  // 设置 use_monotonic = 1
  detect_monotonic();
  // 获取 base 当前时间
  // base->tv_cache.tv_sec 非 0, 则赋给 base->event_tv; 否则获取系统时间
  gettime(base, &base->event_tv);

  // 初始化定时事件小根堆 base->timeheap
  min_heap_ctor(&base->timeheap);
  // 初始化注册事件队列 base->eventqueue
  TAILQ_INIT(&base->eventqueue);
  // 初始化信号事件管理 base->sig
  // socket pair 全双工无名套接字
  base->sig.ev_signal_pair[0] = -1;
  base->sig.ev_signal_pair[1] = -1;

  // 初始化 I/O 多路复用 base->evbase
  base->evbase = NULL;
  // 根据当前系统所支持的类型初始化真正运行的 eventtop 对象
  // 选取 eventops 数组中的第一个 I/O 多路复用机制
  for (i = 0; eventops[i] && !base->evbase; i++) {
    base->evsel = eventops[i];  // 初始化 base->evsel, I/O 多路复用机制使用 epoll

    base->evbase = base->evsel->init(base); // 初始化 base->evbase, 使用 epoll 的 init
  }

  // 判断上一步执行结果, 没有 I/O 多路复用为报错
  if (base->evbase == NULL)
    event_errx(1, "%s: no event mechanism available", __func__);

  // 调用 getenv() 获取环境变量 EVENT_SHOW_METHOD
  if (evutil_getenv("EVENT_SHOW_METHOD"))
    event_msgx("libevent using: %s\n", base->evsel->name);

  // 初始化优先级队列(就绪事件队列), 初始化优先级为 1 (即优先级队列数也为 1)
  // 设置 event base 优先级 base->nactivequeues
  // 分配数组 base->activequeues, 数组大小与优先级相同
  /* allocate a single active event queue */
  event_base_priority_init(base, 1);

  return (base);
}

void event_base_free(struct event_base *base) {
  int i, n_deleted=0;
  struct event *ev;

  if (base == NULL && current_base)
    base = current_base;
  if (base == current_base)
    current_base = NULL;

  /* XXX(niels) - check for internal events first */
  assert(base);
  /* Delete all non-internal events. */
  for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) {
    struct event *next = TAILQ_NEXT(ev, ev_next);
    if (!(ev->ev_flags & EVLIST_INTERNAL)) {
      event_del(ev);
      ++n_deleted;
    }
    ev = next;
  }
  while ((ev = min_heap_top(&base->timeheap)) != NULL) {
    event_del(ev);
    ++n_deleted;
  }

  for (i = 0; i < base->nactivequeues; ++i) {
    for (ev = TAILQ_FIRST(base->activequeues[i]); ev; ) {
      struct event *next = TAILQ_NEXT(ev, ev_active_next);
      if (!(ev->ev_flags & EVLIST_INTERNAL)) {
        event_del(ev);
        ++n_deleted;
      }
      ev = next;
    }
  }

  if (n_deleted)
    event_debug(("%s: %d events were still set in base",
      __func__, n_deleted));

  if (base->evsel->dealloc != NULL)
    base->evsel->dealloc(base, base->evbase);

  for (i = 0; i < base->nactivequeues; ++i)
    assert(TAILQ_EMPTY(base->activequeues[i]));

  assert(min_heap_empty(&base->timeheap));
  min_heap_dtor(&base->timeheap);

  for (i = 0; i < base->nactivequeues; ++i)
    free(base->activequeues[i]);
  free(base->activequeues);

  assert(TAILQ_EMPTY(&base->eventqueue));

  free(base);
}

/* reinitialized the event base after a fork */
int event_reinit(struct event_base *base) {
  const struct eventop *evsel = base->evsel;
  void *evbase = base->evbase;
  int res = 0;
  struct event *ev;

#if 0
  /* Right now, reinit always takes effect, since even if the
     backend doesn't require it, the signal socketpair code does.
   */
  /* check if this event mechanism requires reinit */
  if (!evsel->need_reinit)
    return (0);
#endif

  /* prevent internal delete */
  if (base->sig.ev_signal_added) {
    /* we cannot call event_del here because the base has
     * not been reinitialized yet. */
    event_queue_remove(base, &base->sig.ev_signal,
        EVLIST_INSERTED);
    if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)
      event_queue_remove(base, &base->sig.ev_signal,
          EVLIST_ACTIVE);
    base->sig.ev_signal_added = 0;
  }

  if (base->evsel->dealloc != NULL)
    base->evsel->dealloc(base, base->evbase);
  evbase = base->evbase = evsel->init(base);
  if (base->evbase == NULL)
    event_errx(1, "%s: could not reinitialize event mechanism",
        __func__);

  TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {
    if (evsel->add(evbase, ev) == -1)
      res = -1;
  }

  return (res);
}

// 当 ev 处于就绪状态时, 不能设置
int event_priority_init(int npriorities) {
  return event_base_priority_init(current_base, npriorities);
}

// event base 优先级初始化, 值越小优先级越高
int event_base_priority_init(struct event_base *base, int npriorities) {
  int i;

  // 当前base 上有就绪的 event, 不能设置优先级
  if (base->event_count_active)
    return (-1);

  // 设置的优先级和当前优先级相同, 直接返回
  if (npriorities == base->nactivequeues)
    return (0);

  // 不同, 则先释放原有的 base->nactivequeues
  if (base->nactivequeues) {
    for (i = 0; i < base->nactivequeues; ++i) {
      free(base->activequeues[i]);
    }

    free(base->activequeues);
  }

  /* Allocate our priority queues */
  base->nactivequeues = npriorities;  // 设置优先级, 就绪队列数与优先级相同
  // 在堆上分配就绪队列数组
  base->activequeues = (struct event_list **)
      calloc(base->nactivequeues, sizeof(struct event_list *));
  if (base->activequeues == NULL)
    event_err(1, "%s: calloc", __func__);

  // 为就绪队列数组中的每个成员队列(链表)分配内存空间并初始化
  for (i = 0; i < base->nactivequeues; ++i) {
    base->activequeues[i] = malloc(sizeof(struct event_list));
    if (base->activequeues[i] == NULL)
      event_err(1, "%s: malloc", __func__);
    TAILQ_INIT(base->activequeues[i]);
  }

  return (0);
}

int event_haveevents(struct event_base *base) {
  return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */
// 就绪事件处理
// 1. 从 base->nactivequeues 中取出优先级最高的就绪事件队列
// 2. 循环处理就绪事件队列
//      2.1 根据事件是否持久化, 从就绪事件队列删除或直接删除事件
//      2.2 调用事件处理回调函数处理事件
static void event_process_active(struct event_base *base) {
  struct event *ev;
  struct event_list *activeq = NULL;
  int i;
  short ncalls;

  // MAJOR: 取出优先级最高的第一个就绪事件队列
  for (i = 0; i < base->nactivequeues; ++i) {
    if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
      activeq = base->activequeues[i];
      break;
    }
  }

  assert(activeq != NULL);

  // 调用事件注册回调函数处理就绪事件队列事件
  for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {
    // 持久化事件, 从就绪事件队列中删除
    // 非持续久化事件, 直接从事件队列中删除
    if (ev->ev_events & EV_PERSIST) {
      event_queue_remove(base, ev, EVLIST_ACTIVE);
    } else {
      event_del(ev);
    }

    /* Allows deletes to work */
    ncalls = ev->ev_ncalls;
    ev->ev_pncalls = &ncalls;
    while (ncalls) {
      ncalls--;
      ev->ev_ncalls = ncalls;

      // MAJOR: 调用事件处理回调函数, 执行处理. ev_res 保存返回值
      (*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);

      if (event_gotsig || base->event_break) {
        ev->ev_pncalls = NULL;
        return;
      }
    }

    ev->ev_pncalls = NULL;
  }
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

int event_dispatch(void) {
  return (event_loop(0));
}

int event_base_dispatch(struct event_base *event_base) {
  return (event_base_loop(event_base, 0));
}

const char *event_base_get_method(struct event_base *base) {
  assert(base);
  return (base->evsel->name);
}

static void event_loopexit_cb(int fd, short what, void *arg) {
  struct event_base *base = arg;
  base->event_gotterm = 1;
}

/* not thread safe */
int event_loopexit(const struct timeval *tv) {
  return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
        current_base, tv));
}

int event_base_loopexit(struct event_base *event_base, const struct timeval *tv) {
  return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,
        event_base, tv));
}

/* not thread safe */
int event_loopbreak(void) {
  return (event_base_loopbreak(current_base));
}

int event_base_loopbreak(struct event_base *event_base) {
  if (event_base == NULL)
    return (-1);

  event_base->event_break = 1;
  return (0);
}

/* not thread safe */

int event_loop(int flags) {
  return event_base_loop(current_base, flags);
}

// 调用对应 I/O 多路复用 dispatch(), 在 event_process_active() 中调用注册回调函数处理
int event_base_loop(struct event_base *base, int flags) {
  const struct eventop *evsel = base->evsel;  // event base 的 I/O 多路复用机制
  void *evbase = base->evbase;  // event base 的 I/O 多路复用实例, epollop 对象
  struct timeval tv;
  struct timeval *tv_p;
  int res, done;

  // 清除时间缓存, 后面 gettime() 获取当前系统时间而非缓存时间
  /* clear time cache */
  base->tv_cache.tv_sec = 0;

  // 处理信号事件时, 指定信号所属的 event base
  // evsignal_base 全局变量, 在处理信号事件时, 用于指明信号所属的 event_base 实例
  // 后续关于信号的一些操作默认使用此 base
  if (base->sig.ev_signal_added) {
    evsignal_base = base;
  }

  // 事件主循环
  done = 0;
  while (!done) {
    // 中断循环标识
    // event_loopexit_cb() 可设置 event_gotterm 标志
    /* Terminate the loop if we have been asked to */
    if (base->event_gotterm) {
      base->event_gotterm = 0;
      break;
    }

    // 中断循环标识
    // event_base_loopbreak() 可设置
    if (base->event_break) {
      base->event_break = 0;
      break;
    }

    /* You cannot use this interface for multi-threaded apps */
    // 当 event_gotsig 被设置时, 则 event_sigcb 就是信号处理的回调函数
    while (event_gotsig) {
      event_gotsig = 0;
      if (event_sigcb) {
        res = (*event_sigcb)(); // 调用信号处理回调函数
        if (res == -1) {
          errno = EINTR;
          return (-1);
        }
      }
    }

    // 获取精确 timeout, 校准时间
    // 采用 monotonic 时钟类型时此函数不起作用
    // 比较 last wait time 和当前时间, 如果当前时间小于 last wait time, 表明时间有问题
    // 需要更新 timerheap 中所有定时时间的超时时间
    timeout_correct(base, &tv);

    tv_p = &tv;
    // 当前如果没有就绪事件, 获取接下来的最小等待时间
    // 有就绪事件, 无需等待 epoll_wait, 清空定时器, 需要立刻返回
    if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) {
      // MAJOR: 根据定时器堆中最小超时时间计算 I/O 多路复用的最大等待时间 tv_p
      // 根据所有 timer 事件的最小超时时间来设置 epoll 的 timeout 时间, 当 epoll 返回时
      // 再激活所有就绪的 timer 事件就可以了, 这样就将 timer 事件完美融合到 epoll 中
      // timeheap 获取最小值的时间复杂度为 O(1)
      timeout_next(base, &tv_p);
    } else {
      /*
       * if we have active events, we just poll new events
       * without waiting.
       */
      evutil_timerclear(&tv);
    }

    // 当前无事件注册, 直接退出
    /* If we have no events, we just exit */
    if (!event_haveevents(base)) {
      event_debug(("%s: no events registered.", __func__));
      return (1);
    }

    // 更新 last wait time 时间
    /* update last old time */
    gettime(base, &base->event_tv);
    // 清除时间缓存
    /* clear time cache */
    base->tv_cache.tv_sec = 0;

    // 内部使用 epoll_wait() 等待事件, 仅处理读写事件, 信号事件在 evsignal_process() 处理
    // MAJOR: 调用 I/O 多路复用, 监听事件, 将就绪事件添加到就绪事件队列
    // epoll_wait() 会阻塞 tv_p 时间, 超时或有事件就绪时, 函数返回,
    // 此时 base->nactivequeues 中就包含就绪事件
    res = evsel->dispatch(base, evbase, tv_p);

    if (res == -1) {
      return (-1);
    }

    // TODO: 是否是为了保持在当前处理中时间不变?
    // 将 time cache 赋值为当前系统时间, 缓存的作用是无需调用系统调用获取时间, 省时
    gettime(base, &base->tv_cache);

    // 从 timeheap 小根堆中获取超时事件, 将其从 timeheap 中删除, 同时加入到就绪队列
    timeout_process(base);

    // 就绪事件处理
    if (base->event_count_active) {
      // MAJOR: 就绪事件处理
      // 根据优先级获取就绪事件队列, 从就绪事件队列中取出事件, 调用注册的回调函数处理事件
      // 处理时会寻找最高优先级(priority 最小) 的就绪事件链表, 优先处理
      // 然后处理链表中的所有就绪事件, 因此低优先级的就绪事件可能得不到及时处理
      event_process_active(base);
      // 每次处理一个事件后, event_count_active--
      // flag 默认传入为 0, 默认 dispatch 是不会推出的
      if (!base->event_count_active && (flags & EVLOOP_ONCE)) {
        done = 1;
      }

    } else if (flags & EVLOOP_NONBLOCK) {
      done = 1;
    }
  }

  // 循环结束, 清空时间缓存
  /* clear time cache */
  base->tv_cache.tv_sec = 0;

  event_debug(("%s: asked to terminate loop.", __func__));
  return (0);
}

/* Sets up an event for processing once */

struct event_once {
  struct event ev;

  void (*cb)(int, short, void *);
  void *arg;
};

/* One-time callback, it deletes itself */

static void event_once_cb(int fd, short events, void *arg) {
  struct event_once *eonce = arg;

  (*eonce->cb)(fd, events, eonce->arg);
  free(eonce);
}

/* not threadsafe, event scheduled once. */
int event_once(int fd, short events, void (*callback)(int, short, void *),
    void *arg, const struct timeval *tv) {
  return event_base_once(current_base, fd, events, callback, arg, tv);
}

/* Schedules an event once */
int event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv) {
  struct event_once *eonce;
  struct timeval etv;
  int res;

  /* We cannot support signals that just fire once */
  if (events & EV_SIGNAL)
    return (-1);

  if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
    return (-1);

  eonce->cb = callback;
  eonce->arg = arg;

  if (events == EV_TIMEOUT) {
    if (tv == NULL) {
      evutil_timerclear(&etv);
      tv = &etv;
    }

    evtimer_set(&eonce->ev, event_once_cb, eonce);
  } else if (events & (EV_READ|EV_WRITE)) {
    events &= EV_READ|EV_WRITE;

    event_set(&eonce->ev, fd, events, event_once_cb, eonce);
  } else {
    /* Bad event combination */
    free(eonce);
    return (-1);
  }

  res = event_base_set(base, &eonce->ev);
  if (res == 0)
    res = event_add(&eonce->ev, tv);
  if (res != 0) {
    free(eonce);
    return (res);
  }

  return (0);
}

// 初始化 event, 设置回调函数, 设置小根堆中索引, 设置 event 优先级
// events 对应 EV_TIMEOUT EV_READ 等
void event_set(struct event *ev, int fd, short events,
    void (*callback)(int, short, void *), void *arg) {
  /* Take the current base - caller needs to set the real base later */
  ev->ev_base = current_base; // 设置 event 属于当前 base, 全局变量 current_base 在 event_init() 中设置

  ev->ev_callback = callback; // 设置回调函数
  ev->ev_arg = arg; // 设置回调函数的传参
  ev->ev_fd = fd;
  ev->ev_events = events;
  ev->ev_res = 0;
  ev->ev_flags = EVLIST_INIT; // 设置 event 状态
  ev->ev_ncalls = 0;
  ev->ev_pncalls = NULL;

  // 小根堆元素初始化索引为 -1
  min_heap_elem_init(ev);

  // 默认设置为中等优先级
  /* by default, we put new events into the middle priority */
  if (current_base) {
    ev->ev_pri = current_base->nactivequeues/2;
  }
}

// 对新建的状态为 EVLIST_INIT 的 event 设置所属 ev_base
// 只有一个事件时, 可以不用设置, event_init() 中已经设置了全局
int event_base_set(struct event_base *base, struct event *ev) {
  /* Only innocent events may be assigned to a different base */
  if (ev->ev_flags != EVLIST_INIT)
    return (-1);

  ev->ev_base = base;
  ev->ev_pri = base->nactivequeues/2;

  return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */
// 设置 event 优先级, 但是不能对 EVLIST_ACTIVE event 设置
int event_priority_set(struct event *ev, int pri) {
  if (ev->ev_flags & EVLIST_ACTIVE) {
    return (-1);
  }

  if (pri < 0 || pri >= ev->ev_base->nactivequeues) {
    return (-1);
  }

  ev->ev_pri = pri;

  return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */

int event_pending(struct event *ev, short event, struct timeval *tv) {
  struct timeval	now, res;
  int flags = 0;

  if (ev->ev_flags & EVLIST_INSERTED)
    flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL));
  if (ev->ev_flags & EVLIST_ACTIVE)
    flags |= ev->ev_res;
  if (ev->ev_flags & EVLIST_TIMEOUT)
    flags |= EV_TIMEOUT;

  event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

  /* See if there is a timeout that we should report */
  if (tv != NULL && (flags & event & EV_TIMEOUT)) {
    gettime(ev->ev_base, &now);
    evutil_timersub(&ev->ev_timeout, &now, &res);
    /* correctly remap to real time */
    evutil_gettimeofday(&now, NULL);
    evutil_timeradd(&now, &res, tv);
  }

  return (flags & event);
}

// 相当于 Reactor::register_handler() 注册事件
// 将 event 添加到 events 事件队列, 注册事件
// 1. 将 event 注册到 event_base 的 I/O 多路复用要监听的事件链表中
// 2. 将 event 注册到 event_base 的 EVLIST_INSERTED 已注册事件队列中
// 3. 如果传入超时时间, 则删除旧的超时时间, 重新设置, 并将 event 添加到 event_base 的
//    小根堆中; 没有超时时间, 则不添加到小根堆中
int event_add(struct event *ev, const struct timeval *tv) {
  struct event_base *base = ev->ev_base;  // event 要注册的 event base
  const struct eventop *evsel = base->evsel;  // event base 的 I/O 多路复用机制
  void *evbase = base->evbase;  // event base I/O 多路复用机制实例, 使用 epoll
  int res = 0;

  event_debug((
     "event_add: event: %p, %s%s%scall %p",
     ev,
     ev->ev_events & EV_READ ? "EV_READ " : " ",
     ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
     tv ? "EV_TIMEOUT " : " ",
     ev->ev_callback));

  assert(!(ev->ev_flags & ~EVLIST_ALL));

  /*
   * prepare for timeout insertion further below, if we get a
   * failure on any step, we should not change any state.
   */
  // 传入超时时间, 且 event 不在小根堆上, 则在小根堆上预留一个位置
  // 小根堆会根据需要是否执行扩容
  // 新的 timer 事件, 调用 timer heap 接口在堆上预留一个位置. 保证该操作的原子性
  // 向 epoll 注册可能会失败, 而当在堆上预留成功后, 定时事件的添加肯定不会失败; 预留位置的可
  // 能结果时堆扩充, 但是内部元素不会改变
  if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
    if (min_heap_reserve(&base->timeheap, 1 + min_heap_size(&base->timeheap)) == -1) {
      return (-1);  /* ENOMEM == errno */
    }
  }

  // event 不在已注册事件队列或就绪事件队列中
  // I/O 事件及信号事件执行注册到 epoll, 并将事件加入注册事件队列 base->eventqueue
  if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) &&
      !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) {
    // 将 event 注册到 I/O 多路复用要监听的事件队列中
    res = evsel->add(evbase, ev);
    if (res != -1) {
      // event 注册监听事件成功, 则将 event 添加到已注册事件队列
      event_queue_insert(base, ev, EVLIST_INSERTED);
    }
  }

  /*
   * we should change the timeout state only if the previous event
   * addition succeeded.
   */
  // 准备添加定时事件
  // 前面事件添加成功, 且传入超时时间, 则修改 timeout 状态, 删除旧的超时时间, 重新设置, 并
  // 将 event 添加到 event_base 小根堆中; 没有超时时间则不添加到小根堆中
  if (res != -1 && tv != NULL) {
    struct timeval now;

    /*
     * we already reserved memory above for the case where we
     * are not replacing an exisiting timeout.
     */
    // EVLIST_TIMEOUT 表明 event 已经在定时器堆中
    // 当前事件在 time 堆中, 则将其从小根堆中清除
    if (ev->ev_flags & EVLIST_TIMEOUT) {
      event_queue_remove(base, ev, EVLIST_TIMEOUT);
    }

    /* Check if it is active due to a timeout.  Rescheduling
     * this timeout before the callback can be executed
     * removes it from the active list. */
    // 判断事件是否由于已超时而处于就绪列表中, 在执行回调前将其从就绪列表中删除
    // 这里是因为 event_add 函数会在事件被触发后再次调用, 此时就需要先将事件从就绪列表中删除
    if ((ev->ev_flags & EVLIST_ACTIVE) &&
        (ev->ev_res & EV_TIMEOUT)) {
      /* See if we are just active executing this
       * event in a loop
       */
      if (ev->ev_ncalls && ev->ev_pncalls) {
        /* Abort loop */
        *ev->ev_pncalls = 0;  // 调用次数清 0
      }

      // 从就绪事件队列中删除
      event_queue_remove(base, ev, EVLIST_ACTIVE);
    }

    // 根据 tv 值, 重新设置事件超时时间, 并插入到 timer 小根堆中
    gettime(base, &now);
    evutil_timeradd(&now, tv, &ev->ev_timeout);

    event_debug((
       "event_add: timeout in %ld seconds, call %p",
       tv->tv_sec, ev->ev_callback));

    // 将事件加入到小根堆中
    event_queue_insert(base, ev, EVLIST_TIMEOUT);
  }

  return (res);
}

// 删除 event 事件
// 主要调用 event_queue_remove() 完成删除
// 对于 I/O 时间, 从 I/O demultiplexer 上将事件注销; 对于信号事件, 将从信号事件链表删除,
// 对于定时事件, 将从堆上删除
int event_del(struct event *ev) {
  struct event_base *base;
  const struct eventop *evsel;
  void *evbase;

  event_debug(("event_del: %p, callback %p", ev, ev->ev_callback));

  /* An event without a base has not been added */
  if (ev->ev_base == NULL) {
    return (-1);
  }

  base = ev->ev_base;
  evsel = base->evsel;
  evbase = base->evbase;

  assert(!(ev->ev_flags & ~EVLIST_ALL));

  // ev_callback 调用次数置为 0
  /* See if we are just active executing this event in a loop */
  if (ev->ev_ncalls && ev->ev_pncalls) {
    /* Abort loop */
    *ev->ev_pncalls = 0;  // 计数清 0
  }

  // 从对应链表中删除事件
  if (ev->ev_flags & EVLIST_TIMEOUT) {
    event_queue_remove(base, ev, EVLIST_TIMEOUT);
  }


  if (ev->ev_flags & EVLIST_ACTIVE) {
    event_queue_remove(base, ev, EVLIST_ACTIVE);
  }

  if (ev->ev_flags & EVLIST_INSERTED) {
    event_queue_remove(base, ev, EVLIST_INSERTED);

    // EVLIST_INSERTED 表明是 I/O 或信号事件, 需要调用 I/O demultiplexer 注销事件
    return (evsel->del(evbase, ev));  // 从 I/O 多路复用监听的事件中删除
  }

  return (0);
}

// 向就绪事件队列加入事件
void event_active(struct event *ev, int res, short ncalls) {
  /* We get different kinds of events, add them together */
  if (ev->ev_flags & EVLIST_ACTIVE) {
    ev->ev_res |= res;
    return;
  }

  ev->ev_res = res;
  ev->ev_ncalls = ncalls;
  ev->ev_pncalls = NULL;
  // 将事件加入就绪事件队列
  event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

// 获取接下来最小的等待时间(即 最近即将超时事件的超时时间 - 当前时间)
static int timeout_next(struct event_base *base, struct timeval **tv_p) {
  struct timeval now;
  struct event *ev;
  struct timeval *tv = *tv_p;

  // 没有基于时间的事件就绪, 直接返回
  // 有则获取小根堆 root 节点, 即最小值的节点
  if ((ev = min_heap_top(&base->timeheap)) == NULL) {
    /* if no time-based events are active wait for I/O */
    *tv_p = NULL;
    return (0);
  }

  // 获取 base 时间, 有缓存优先使用缓存事件, 没有则获取当前时间
  if (gettime(base, &now) == -1)
    return (-1);

  // 事件超时时间小于等于当前时间, 即已经超时, 清除定时器
  if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {
    evutil_timerclear(tv);
    return (0);
  }

  // 事件未超时, 获取距离超时的剩余时间
  evutil_timersub(&ev->ev_timeout, &now, tv);

  assert(tv->tv_sec >= 0);
  assert(tv->tv_usec >= 0);

  event_debug(("timeout_next: in %ld seconds", tv->tv_sec));
  return (0);
}

/*
 * Determines if the time is running backwards by comparing the current
 * time against the last time we checked.  Not needed when using clock
 * monotonic.
 */

static void timeout_correct(struct event_base *base, struct timeval *tv) {
  struct event **pev;
  unsigned int size;
  struct timeval off;

  if (use_monotonic)
    return;

  /* Check if time is running backwards */
  gettime(base, tv);
  // 比较 last wait time 和当前时间, 如果当前时间小于 last wait time, 表明时间有问题
  // 需要更新 timerheap 中所有定时时间的超时时间
  if (evutil_timercmp(tv, &base->event_tv, >=)) {
    base->event_tv = *tv;
    return;
  }

  event_debug(("%s: time is running backwards, corrected",
        __func__));
  evutil_timersub(&base->event_tv, tv, &off);

  /*
   * We can modify the key element of the node without destroying
   * the key, beause we apply it to all in the right order.
   */
  pev = base->timeheap.p;
  size = base->timeheap.n;
  for (; size-- > 0; ++pev) {
    struct timeval *ev_tv = &(**pev).ev_timeout;
    evutil_timersub(ev_tv, &off, ev_tv);
  }
  /* Now remember what the new time turned out to be. */
  base->event_tv = *tv;
}

// 超时处理
void timeout_process(struct event_base *base) {
  struct timeval now;
  struct event *ev;

  if (min_heap_empty(&base->timeheap))
    return;

  gettime(base, &now);

  // 循环从小根堆中获取事件, 判断是否超时, 如果超时则从 I/O 队列中将其删除, 并将其加入到
  // 就绪事件队列
  while ((ev = min_heap_top(&base->timeheap))) {
    if (evutil_timercmp(&ev->ev_timeout, &now, >)) {
      break;
    }

    /* delete this event from the I/O queues */
    event_del(ev);  // 从 I/O 队列中删除事件

    event_debug(("timeout_process: call %p", ev->ev_callback));
    // MAJOR: 事件就绪, 加入就绪队列
    event_active(ev, EV_TIMEOUT, 1);
  }
}

// 根据 queue 类型, 从相应的队列中删除事件
void event_queue_remove(struct event_base *base, struct event *ev, int queue) {
  if (!(ev->ev_flags & queue)) {
    event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
      ev, ev->ev_fd, queue);
  }

  if (~ev->ev_flags & EVLIST_INTERNAL) {
    base->event_count--;
  }

  ev->ev_flags &= ~queue;
  switch (queue) {
  case EVLIST_INSERTED:
    TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
    break;
  case EVLIST_ACTIVE:
    base->event_count_active--;
    TAILQ_REMOVE(base->activequeues[ev->ev_pri], ev, ev_active_next);
    break;
  case EVLIST_TIMEOUT:
    min_heap_erase(&base->timeheap, ev);
    break;
  default:
    event_errx(1, "%s: unknown queue %x", __func__, queue);
  }
}

// 根据 queue 的类型, 将事件加入对应的队列/小根堆中, 并增加相应的事件计数, 更新 event 状态
void event_queue_insert(struct event_base *base, struct event *ev, int queue) {
  // ev 可能已经在激活列表中, 避免重复插入
  if (ev->ev_flags & queue) {
    /* Double insertion is possible for active events */
    if (queue & EVLIST_ACTIVE) {
      return;
    }

    event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
         ev, ev->ev_fd, queue);
  }

  // 当前状态不是 EVLIST_INTERNAL 时, 增加 event base 事件计数
  if (~ev->ev_flags & EVLIST_INTERNAL) {
    base->event_count++;
  }

  ev->ev_flags |= queue;  // 记录更新 event 状态
  switch (queue) {
  case EVLIST_INSERTED: // I/O 或信号事件, 加入已注册事件链表
    TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
    break;
  case EVLIST_ACTIVE: // 就绪事件, 加入就绪事件链表
    base->event_count_active++; // 增加就绪事件计数
    TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri], ev,ev_active_next);
    break;
  case EVLIST_TIMEOUT: {  // 定时事件, 加入时间堆
    min_heap_push(&base->timeheap, ev);
    break;
  }
  default:
    event_errx(1, "%s: unknown queue %x", __func__, queue);
  }
}

/* Functions for debugging */

const char *event_get_version(void) {
  return (VERSION);
}

/*
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */

const char *event_get_method(void) {
  return (current_base->evsel->name);
}
