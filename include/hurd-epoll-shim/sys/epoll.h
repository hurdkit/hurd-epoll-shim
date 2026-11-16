/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Written from the Linux epoll(7), epoll_ctl(2) and epoll_wait(2) manual
 * pages; constant values match Linux <linux/eventpoll.h>. */
#ifndef HURD_EPOLL_SHIM_SYS_EPOLL_H
#define HURD_EPOLL_SHIM_SYS_EPOLL_H

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#ifdef __GLIBC__ /* sigset_t even in strict ISO C mode */
#include <bits/types/sigset_t.h>
#endif

#include <epoll-shim/wrap.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPOLL_CLOEXEC O_CLOEXEC

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN 0x001u
#define EPOLLPRI 0x002u
#define EPOLLOUT 0x004u
#define EPOLLERR 0x008u
#define EPOLLHUP 0x010u
#define EPOLLNVAL 0x020u
#define EPOLLRDNORM 0x040u
#define EPOLLRDBAND 0x080u
#define EPOLLWRNORM 0x100u
#define EPOLLWRBAND 0x200u
#define EPOLLMSG 0x400u
#define EPOLLRDHUP 0x2000u
#define EPOLLEXCLUSIVE (1u << 28)
#define EPOLLWAKEUP (1u << 29)
#define EPOLLONESHOT (1u << 30)
#define EPOLLET (1u << 31)

typedef union epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
} epoll_data_t;

struct epoll_event {
	uint32_t events;
	epoll_data_t data;
}
#ifdef __x86_64__
__attribute__((__packed__)) /* same layout as Linux x86_64 */
#endif
;

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
    int timeout);
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
    int timeout, const sigset_t *sigmask);

#ifdef __cplusplus
}
#endif

#endif
