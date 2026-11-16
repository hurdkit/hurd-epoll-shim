/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Written from the Linux eventfd(2) manual page. */
#ifndef HURD_EPOLL_SHIM_SYS_EVENTFD_H
#define HURD_EPOLL_SHIM_SYS_EVENTFD_H

#include <fcntl.h>
#include <stdint.h>

#include <epoll-shim/wrap.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC O_CLOEXEC
#define EFD_NONBLOCK O_NONBLOCK

int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#ifdef __cplusplus
}
#endif

#endif
