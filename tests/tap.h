/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Minimal TAP helpers shared by the tests.  The same test sources are built
 * against the shim (HURD_EPOLL_SHIM defined by its headers) and, as a
 * control, against the Linux kernel implementation. */
#ifndef TAP_H
#define TAP_H

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HURD_EPOLL_SHIM
#define SHIM 1
#else
#define SHIM 0
#endif

static int tap_planned, tap_count, tap_failed;

static inline void plan(int n)
{
	tap_planned = n;
	printf("1..%d\n", n);
	fflush(stdout);
}

static inline int ok(int cond, const char *fmt, ...)
{
	int saved = errno;
	va_list ap;

	tap_count++;
	if (!cond)
		tap_failed++;
	printf("%sok %d - ", cond ? "" : "not ", tap_count);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	if (!cond)
		printf("\n# errno %d (%s)", saved, strerror(saved));
	printf("\n");
	fflush(stdout);
	errno = saved;
	return cond;
}

static inline void skip(int n, const char *why)
{
	while (n-- > 0)
		printf("ok %d # SKIP %s\n", ++tap_count, why);
	fflush(stdout);
}

static inline int done(void)
{
	if (tap_count != tap_planned) {
		printf("# planned %d tests but ran %d\n", tap_planned,
		    tap_count);
		return 1;
	}
	return tap_failed != 0;
}

static inline int64_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static inline void sleep_ms(int ms)
{
	struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000 };
	while (nanosleep(&t, &t) < 0 && errno == EINTR)
		;
}

/* 1 if fd is readable within ms milliseconds, 0 if not, -1 on error. */
static inline int pollin(int fd, int ms)
{
	struct pollfd p = { fd, POLLIN, 0 };
	int r = poll(&p, 1, ms);
	if (r < 0)
		return -1;
	return r == 1 && (p.revents & POLLIN) ? 1 : 0;
}

#endif
