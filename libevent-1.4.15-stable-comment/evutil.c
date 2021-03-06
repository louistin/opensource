/*
 * Copyright (c) 2007 Niels Provos <provos@citi.umich.edu>
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
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <signal.h>

#include <sys/queue.h>
#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

// 创建一对无名的, 相互连接的套接字
// 这对套接字可用于全双工通信, 每个套接字可读也可写. fd[0] 写入只能在 fd[1] 读取, 或者反之
int evutil_socketpair(int family, int type, int protocol, int fd[2]) {
  return socketpair(family, type, protocol, fd);
}

int evutil_make_socket_nonblocking(int fd) {
  {
    int flags;
    if ((flags = fcntl(fd, F_GETFL, NULL)) < 0) {
      event_warn("fcntl(%d, F_GETFL)", fd);
      return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
      event_warn("fcntl(%d, F_SETFL)", fd);
      return -1;
    }
  }

  return 0;
}

ev_int64_t
evutil_strtoll(const char *s, char **endptr, int base)
{
#ifdef HAVE_STRTOLL
  return (ev_int64_t)strtoll(s, endptr, base);
#elif SIZEOF_LONG == 8
  return (ev_int64_t)strtol(s, endptr, base);
#elif defined(WIN32) && defined(_MSC_VER) && _MSC_VER < 1300
  /* XXXX on old versions of MS APIs, we only support base
   * 10. */
  ev_int64_t r;
  if (base != 10)
    return 0;
  r = (ev_int64_t) _atoi64(s);
  while (isspace(*s))
    ++s;
  while (isdigit(*s))
    ++s;
  if (endptr)
    *endptr = (char*) s;
  return r;
#elif defined(WIN32)
  return (ev_int64_t) _strtoi64(s, endptr, base);
#else
#error "I don't know how to parse 64-bit integers."
#endif
}

#ifndef _EVENT_HAVE_GETTIMEOFDAY
int evutil_gettimeofday(struct timeval *tv, struct timezone *tz) {
  struct _timeb tb;

  if(tv == NULL)
    return -1;

  _ftime(&tb);
  tv->tv_sec = (long) tb.time;
  tv->tv_usec = ((int) tb.millitm) * 1000;
  return 0;
}
#endif

int
evutil_snprintf(char *buf, size_t buflen, const char *format, ...)
{
  int r;
  va_list ap;
  va_start(ap, format);
  r = evutil_vsnprintf(buf, buflen, format, ap);
  va_end(ap);
  return r;
}

int
evutil_vsnprintf(char *buf, size_t buflen, const char *format, va_list ap)
{
#ifdef _MSC_VER
  int r = _vsnprintf(buf, buflen, format, ap);
  buf[buflen-1] = '\0';
  if (r >= 0)
    return r;
  else
    return _vscprintf(format, ap);
#else
  int r = vsnprintf(buf, buflen, format, ap);
  buf[buflen-1] = '\0';
  return r;
#endif
}

static int
evutil_issetugid(void)
{
#ifdef _EVENT_HAVE_ISSETUGID
  return issetugid();
#else

#ifdef _EVENT_HAVE_GETEUID
  if (getuid() != geteuid())
    return 1;
#endif
#ifdef _EVENT_HAVE_GETEGID
  if (getgid() != getegid())
    return 1;
#endif
  return 0;
#endif
}

const char * evutil_getenv(const char *varname){
  if (evutil_issetugid())
    return NULL;

  return getenv(varname);
}
