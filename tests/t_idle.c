/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Idle CPU: nothing ready must mean no busy loop anywhere. */
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include "tap.h"

static int64_t cpu_ms(void)
{
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return (int64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000 +
	    (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000;
}

int main(void)
{
	struct epoll_event ev = { EPOLLIN, { 0 } }, out[4];
	struct itimerspec far = { .it_value = { 3600, 0 } };
	sigset_t usr2;
	struct itimerspec fast = { .it_interval = { 0, 1000000 },
		.it_value = { 0, 1000000 } };
	int ep, fds[50][2], efd, tfd, sfd, unread, r;
	int64_t c0, t0;

	sigemptyset(&usr2);
	sigaddset(&usr2, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &usr2, NULL);

	plan(4);

	ep = epoll_create1(0);
	for (int i = 0; i < 50; i++) {
		if (pipe(fds[i]))
			abort();
		epoll_ctl(ep, EPOLL_CTL_ADD, fds[i][0], &ev);
	}
	c0 = cpu_ms();
	t0 = now_ms();
	r = epoll_wait(ep, out, 4, 2000);
	t0 = now_ms() - t0;
	c0 = cpu_ms() - c0;
	ok(r == 0 && t0 >= 1900 && c0 < 50,
	    "epoll_wait 2 s on 50 idle pipes: %lld ms CPU", (long long)c0);

	c0 = cpu_ms();
	r = pollin(ep, 2000);
	c0 = cpu_ms() - c0;
	ok(r == 0 && c0 < 50, "raw poll 2 s on the epoll fd: %lld ms CPU",
	    (long long)c0);

	efd = eventfd(0, 0);
	tfd = timerfd_create(CLOCK_MONOTONIC, 0);
	timerfd_settime(tfd, 0, &far, NULL);
	sfd = signalfd(-1, &usr2, 0);
	/* a 1 ms periodic timer that nobody reads must not cost CPU either */
	unread = timerfd_create(CLOCK_MONOTONIC, 0);
	timerfd_settime(unread, 0, &fast, NULL);
	epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
	epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
	epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
	{
		int outer = epoll_create1(0);
		epoll_ctl(outer, EPOLL_CTL_ADD, ep, &ev);
		c0 = cpu_ms();
		r = epoll_wait(outer, out, 4, 2000);
		c0 = cpu_ms() - c0;
		ok(r == 0 && c0 < 50,
		    "nested epoll, idle eventfd/timerfd/signalfd, unread 1 ms "
		    "timer: %lld ms CPU",
		    (long long)c0);
		close(outer);
	}
	close(efd);
	close(tfd);
	close(sfd);
	close(unread);
	for (int i = 0; i < 50; i++) {
		close(fds[i][0]);
		close(fds[i][1]);
	}
	ok(close(ep) == 0, "close");
	return done();
}
