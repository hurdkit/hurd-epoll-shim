/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/*
 * Interposition of read/write/close/fcntl for hurd-epoll-shim descriptors.
 * Included by every public header.  Define EPOLL_SHIM_DISABLE_WRAPPER_MACROS
 * before including to get the declarations without the macros.
 */
#ifndef HURD_EPOLL_SHIM_WRAP_H
#define HURD_EPOLL_SHIM_WRAP_H

/* Pull in the real prototypes before the macros are defined. */
#include <fcntl.h>
#include <unistd.h>

#define HURD_EPOLL_SHIM 1

#ifdef __cplusplus
extern "C" {
#endif

ssize_t epoll_shim_read(int fd, void *buf, size_t count);
ssize_t epoll_shim_write(int fd, const void *buf, size_t count);
int epoll_shim_close(int fd);
int epoll_shim_fcntl(int fd, int cmd, ...);
const char *epoll_shim_version(void);

#ifdef __cplusplus
}
#endif

#ifndef EPOLL_SHIM_DISABLE_WRAPPER_MACROS
#define read(...) epoll_shim_read(__VA_ARGS__)
#define write(...) epoll_shim_write(__VA_ARGS__)
#define close(...) epoll_shim_close(__VA_ARGS__)
#define fcntl(...) epoll_shim_fcntl(__VA_ARGS__)
#endif

#endif
