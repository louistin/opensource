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
#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

typedef void (*ev_sighandler_t)(int);

struct evsignal_info {
  // 为 socket pair 的读 socket 向 event_base 注册读事件时使用的 event 结构体
  struct event ev_signal;
  // 由 evutil_socketpair() 创建, 0 socket 负责写, 1 负责读
  int ev_signal_pair[2];
  // 记录 ev_signal 事件是否已注册
  int ev_signal_added;
  // 信号是否发生标志
  volatile sig_atomic_t evsignal_caught;
  // 数组, evsigevents[signo] 表示注册到信号 signo 的事件链表
  struct event_list evsigevents[NSIG];
  // sig_atomic_t 信号处理程序中作为变量使用, 该对象可作为一个原子实体访问
  sig_atomic_t evsigcaught[NSIG];   // 每个数组成员为对应的信号触发的次数
#ifdef HAVE_SIGACTION
  // 记录原来的 signal 处理函数指针, 当 signo 注册的 event 被清空时, 需要重新设置其处理函数
  struct sigaction **sh_old;
#else
  ev_sighandler_t **sh_old;
#endif
  int sh_old_max;
};
int evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
