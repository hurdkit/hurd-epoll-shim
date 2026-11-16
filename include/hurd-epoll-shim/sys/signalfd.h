/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Written from the Linux signalfd(2) manual page; the record layout is the
 * 128-byte struct signalfd_siginfo documented there. */
#ifndef HURD_EPOLL_SHIM_SYS_SIGNALFD_H
#define HURD_EPOLL_SHIM_SYS_SIGNALFD_H

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

#define SFD_CLOEXEC O_CLOEXEC
#define SFD_NONBLOCK O_NONBLOCK

struct signalfd_siginfo {
	uint32_t ssi_signo;
	int32_t ssi_errno;
	int32_t ssi_code;
	uint32_t ssi_pid;
	uint32_t ssi_uid;
	int32_t ssi_fd;
	uint32_t ssi_tid;
	uint32_t ssi_band;
	uint32_t ssi_overrun;
	uint32_t ssi_trapno;
	int32_t ssi_status;
	int32_t ssi_int;
	uint64_t ssi_ptr;
	uint64_t ssi_utime;
	uint64_t ssi_stime;
	uint64_t ssi_addr;
	uint16_t ssi_addr_lsb;
	uint16_t __pad2;
	int32_t ssi_syscall;
	uint64_t ssi_call_addr;
	uint32_t ssi_arch;
	uint8_t __pad[28];
};

int signalfd(int fd, const sigset_t *mask, int flags);

#ifdef __cplusplus
}
#endif

#endif
