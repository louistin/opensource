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
#ifndef _EVENT_INTERNAL_H_
#define _EVENT_INTERNAL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "min_heap.h"
#include "evsignal.h"

// I/O 多路复用机制相关信息
struct eventop {
  const char *name;
  void *(*init)(struct event_base *); // 初始化
  int (*add)(void *, struct event *); // 注册事件
  int (*del)(void *, struct event *); // 删除事件
  int (*dispatch)(struct event_base *, void *, struct timeval *); // 事件分发
  void (*dealloc)(struct event_base *, void *); // 注销, 释放资源
  /* set if we need to reinitialize the event base */
  int need_reinit;
};

// 相当于 Reactor, 内部使用 EventDemultiplexer 注册, 注销事件, 并运行事件循环, 当有事件
// 就绪时, 调用注册事件的回调函数处理事件. evsel
struct event_base {
  // 封装了 epoll 相关接口信息, EventDemultiplexer
  const struct eventop *evsel;  // I/O 多路复用机制的封装, eventops[] 数组中的一项
  void *evbase; // I/O 多路复用机制的一个实例(eventop 实例对象), 执行具体任务. 由 evsel->init() 初始化
  // 该 event_base 上的总 event 数目
  int event_count;		/* counts number of total events */
  // 该 event_base 上的总就绪 event 数目
  int event_count_active;	/* counts number of active events */

  // 中断循环标识
  int event_gotterm;		/* Set to terminate loop */
  // 中断循环标识
  int event_break;		/* Set to terminate loop immediately */

  /* active event management */
  // 指针数组, activequeues[proiority] 指向优先级为 proiority 的链表, 链表的每个节点都
  //   指向一个优先级为 priority 的就绪事件 event
  // 所有被监控事件就绪 event 都被插入这个 queue, 并在 event.ev_flags 追加 EVLIST_ACTIVE 标志
  struct event_list **activequeues; // 就绪队列数组, 数组下标就是优先级, 越小优先级越高
  int nactivequeues;  // 就绪队列数

  // 管理信号事件
  /* signal handling info */
  struct evsignal_info sig;

  // 所有 add 到 base 的 event 都插入到这个 queue, 并在 event.ev_flags 追加 EVLIST_INSERT 标志
  struct event_list eventqueue; // 队列链表, 保存所有注册事件 event 的指针
  struct timeval event_tv; // 指示了 dispatch() 最新返回的时间, 也就是 I/O 事件就绪的事件

  // 超时 event 管理, min_heap[0] 存放第一个最快要超时的 event 指针
  struct min_heap timeheap; // 管理定时事件小根堆

  struct timeval tv_cache;  // 在 dispatch() 返回后被设置为当前系统时间, 它缓存了
};

/* Internal use only: Functions that might be missing from <sys/queue.h> */
#ifndef HAVE_TAILQFOREACH
#define	TAILQ_FIRST(head)		((head)->tqh_first)
#define	TAILQ_END(head)			NULL
#define	TAILQ_NEXT(elm, field)		((elm)->field.tqe_next)
#define TAILQ_FOREACH(var, head, field)					\
  for((var) = TAILQ_FIRST(head);					\
      (var) != TAILQ_END(head);					\
      (var) = TAILQ_NEXT(var, field))
#define	TAILQ_INSERT_BEFORE(listelm, elm, field) do {			\
  (elm)->field.tqe_prev = (listelm)->field.tqe_prev;		\
  (elm)->field.tqe_next = (listelm);				\
  *(listelm)->field.tqe_prev = (elm);				\
  (listelm)->field.tqe_prev = &(elm)->field.tqe_next;		\
} while (0)
#endif /* TAILQ_FOREACH */

int _evsignal_set_handler(struct event_base *base, int evsignal,
        void (*fn)(int));
int _evsignal_restore_handler(struct event_base *base, int evsignal);

/* defined in evutil.c */
const char *evutil_getenv(const char *varname);

#ifdef __cplusplus
}
#endif

#endif /* _EVENT_INTERNAL_H_ */
