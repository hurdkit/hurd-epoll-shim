/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Written from the Linux timerfd_create(2) manual page. */
#ifndef HURD_EPOLL_SHIM_SYS_TIMERFD_H
#define HURD_EPOLL_SHIM_SYS_TIMERFD_H

#include <fcntl.h>
#include <time.h>
#ifdef __GLIBC__ /* clockid_t and struct itimerspec even in strict ISO C */
#include <bits/types/clockid_t.h>
#include <bits/types/struct_itimerspec.h>
#endif

#include <epoll-shim/wrap.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TFD_CLOEXEC O_CLOEXEC
#define TFD_NONBLOCK O_NONBLOCK

#define TFD_TIMER_ABSTIME 1
#define TFD_TIMER_CANCEL_ON_SET 2 /* accepted, never reports ECANCELED */

int timerfd_create(clockid_t clockid, int flags);
int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

#ifdef __cplusplus
}
#endif

#endif
