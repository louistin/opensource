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
    // EVLIST_INTERNAL EV_PERSIST
    struct event ev_signal;
    // ev_signal_pair[1]��Ϊread�����¼�ѭ���м����������źŷ�����ʱ��
    // ͨ��д��ev_signal_pair[0]�����Ի��Ѳ��ص���ev_signal_pair[1]����
    // ���¼��ص��������������ǿ��������лص����¼�ѭ����
    // ִ�У����õ����ж����⣬ͬʱ�ж���źŷ�����ʱ��
    // ������죬��Ϊֻ��Ҫ��ev_signal_pair[1]д��һ���ֽڽ���
    // ֪ͨ���ɣ�memcacheԴ����master�̻߳���worker�̴߳���ͻ�
    // �ͻ�����Ҳ����ô��
    int ev_signal_pair[2];
    // ev_signal�Ƿ��Ѿ�����event loop����
    int ev_signal_added;
    // ��ʾ�Ѿ���׽���źţ�����ʲô�ź�
    volatile sig_atomic_t evsignal_caught;
    // ������ͬ�źŵ�event��ÿ���ź�һ���������event���Լ���ͬһ���ź�
    struct event_list evsigevents[NSIG];

    // ��ͬ�źŲ�׽���Ĵ���
    sig_atomic_t evsigcaught[NSIG];

    // һ���ź�һ���źŴ���ص���HAVE_SIGACTION�Ƿ���sigaction����
    // ���ڱ���֮ǰ���ź���Ϣ�����ź�ֵΪ�±�ȡ��Ӧ��Ԫ��
#ifdef HAVE_SIGACTION
    struct sigaction** sh_old;
#else
    ev_sighandler_t** sh_old;
#endif
    // �ź����͵����ֵ
    int sh_old_max;
};
int evsignal_init(struct event_base*);
void evsignal_process(struct event_base*);
int evsignal_add(struct event*);
int evsignal_del(struct event*);
void evsignal_dealloc(struct event_base*);

#endif /* _EVSIGNAL_H_ */
