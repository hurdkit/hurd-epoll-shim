/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Lifecycle stress, meant to run under ASan/UBSan and TSan.
 * STRESS_SCALE (percent, default 100) shrinks the loop counts. */
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#include "tap.h"

static int scale(int n)
{
	const char *s = getenv("STRESS_SCALE");
	int pct = s ? atoi(s) : 100;
	n = (int)((long)n * pct / 100);
	return n > 0 ? n : 1;
}

static atomic_int stop;

struct shared {
	int ep, efd;
};

static void *poker(void *arg)
{
	struct shared *s = arg;
	uint64_t one = 1;
	while (!atomic_load(&stop)) {
		if (write(s->efd, &one, 8) != 8)
			return (void *)1;
		sleep_ms(1);
	}
	return NULL;
}

static void *waiter(void *arg)
{
	struct shared *s = arg;
	struct epoll_event ev[4];
	uint64_t v;
	while (!atomic_load(&stop)) {
		int r = epoll_wait(s->ep, ev, 4, 20);
		if (r < 0)
			return (void *)1;
		if (r > 0 && read(s->efd, &v, 8) < 0 && errno != EAGAIN)
			return (void *)1;
	}
	return NULL;
}

static void *ctl_thread(void *arg)
{
	struct shared *s = arg;
	while (!atomic_load(&stop)) {
		int p[2];
		struct epoll_event ev = { EPOLLIN, { 0 } };
		if (pipe(p))
			return (void *)1;
		epoll_ctl(s->ep, EPOLL_CTL_ADD, p[0], &ev);
		if (write(p[1], "x", 1) != 1)
			return (void *)1;
		if (p[0] & 1)
			epoll_ctl(s->ep, EPOLL_CTL_DEL, p[0], NULL);
		close(p[0]); /* even fds: closed while still registered */
		close(p[1]);
	}
	return NULL;
}

int main(void)
{
	int n, fails;
	struct itimerspec t = { .it_value = { 0, 100000 } }; /* 100 us */

	plan(5);

	n = scale(10000);
	fails = 0;
	for (int i = 0; i < n; i++) {
		int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
		if (fd < 0 || timerfd_settime(fd, 0, &t, NULL) < 0)
			fails++;
		if (i % 3 == 0)
			sleep_ms(0);
		if (close(fd) < 0)
			fails++;
	}
	ok(fails == 0, "%d timerfd create/arm/close cycles", n);

	n = scale(1000);
	fails = 0;
	for (int i = 0; i < n; i++) {
		int ep = epoll_create1(EPOLL_CLOEXEC), efd = eventfd(1, 0);
		struct epoll_event ev = { EPOLLIN, { 0 } }, out;
		if (ep < 0 || efd < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) < 0 ||
		    epoll_wait(ep, &out, 1, 0) != 1)
			fails++;
		if (close(ep) < 0 || close(efd) < 0)
			fails++;
	}
	ok(fails == 0, "%d epoll create/add/wait/close cycles", n);

	{
		struct shared s;
		struct epoll_event ev = { EPOLLIN, { 0 } };
		pthread_t th[4];
		void *res;
		fails = 0;
		s.ep = epoll_create1(0);
		s.efd = eventfd(0, EFD_NONBLOCK);
		epoll_ctl(s.ep, EPOLL_CTL_ADD, s.efd, &ev);
		pthread_create(&th[0], NULL, poker, &s);
		pthread_create(&th[1], NULL, waiter, &s);
		pthread_create(&th[2], NULL, waiter, &s);
		pthread_create(&th[3], NULL, ctl_thread, &s);
		sleep_ms(scale(1000));
		atomic_store(&stop, 1);
		for (int i = 0; i < 4; i++) {
			pthread_join(th[i], &res);
			fails += res != NULL;
		}
		ok(fails == 0, "threads writing, waiting and changing the set");
		ok(close(s.ep) == 0 && close(s.efd) == 0, "close shared objects");
	}

	{
		/* nested epoll create/destroy with readiness flowing through */
		fails = 0;
		n = scale(300);
		for (int i = 0; i < n; i++) {
			int a = epoll_create1(0), b = epoll_create1(0);
			int e = eventfd(1, 0);
			struct epoll_event ev = { EPOLLIN, { 0 } }, out;
			epoll_ctl(a, EPOLL_CTL_ADD, e, &ev);
			epoll_ctl(b, EPOLL_CTL_ADD, a, &ev);
			if (epoll_wait(b, &out, 1, 1000) != 1)
				fails++;
			close(i & 1 ? a : b);
			close(e);
			close(i & 1 ? b : a);
		}
		ok(fails == 0, "%d nested epoll lifecycles", n);
	}
	return done();
}
