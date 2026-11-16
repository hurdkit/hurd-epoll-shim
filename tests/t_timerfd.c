/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "tap.h"

static struct itimerspec its(long v_ms, long i_ms)
{
	struct itimerspec t;
	t.it_value.tv_sec = v_ms / 1000;
	t.it_value.tv_nsec = (v_ms % 1000) * 1000000;
	t.it_interval.tv_sec = i_ms / 1000;
	t.it_interval.tv_nsec = (i_ms % 1000) * 1000000;
	return t;
}

static int64_t ts_ms(struct timespec t)
{
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* absolute CLOCK_MONOTONIC time now + delta_ms */
static struct itimerspec abs_its(long delta_ms, long i_ms)
{
	struct timespec now;
	int64_t ns;
	struct itimerspec t = its(0, i_ms);
	clock_gettime(CLOCK_MONOTONIC, &now);
	ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec +
	    (int64_t)delta_ms * 1000000;
	t.it_value.tv_sec = ns / 1000000000;
	t.it_value.tv_nsec = ns % 1000000000;
	return t;
}

int main(void)
{
	struct itimerspec t, old;
	uint64_t v;
	int fd, r;
	int64_t t0;

	plan(36);

	errno = 0;
	ok(timerfd_create(12345, 0) == -1 && errno == EINVAL,
	    "unknown clock -> EINVAL");
	ok(timerfd_create(CLOCK_MONOTONIC, 2) == -1 && errno == EINVAL,
	    "unknown create flag -> EINVAL");
	fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
	ok(fd >= 0 && ((fcntl)(fd, F_GETFD) & FD_CLOEXEC),
	    "CLOCK_REALTIME with TFD_CLOEXEC");
	close(fd);
#ifdef CLOCK_BOOTTIME
	fd = timerfd_create(CLOCK_BOOTTIME, 0);
	ok(fd >= 0, "CLOCK_BOOTTIME is accepted");
	close(fd);
#else
	skip(1, "no CLOCK_BOOTTIME");
#endif

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	ok(fd >= 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK),
	    "TFD_NONBLOCK reported by F_GETFL");
	t = its(50, 0);
	ok(timerfd_settime(fd, 4, &t, NULL) == -1 && errno == EINVAL,
	    "unknown settime flag -> EINVAL");
	t.it_value.tv_nsec = 1000000000;
	ok(timerfd_settime(fd, 0, &t, NULL) == -1 && errno == EINVAL,
	    "tv_nsec out of range -> EINVAL");
	{
		char small[4];
		ok(read(fd, small, 4) == -1 && errno == EINVAL,
		    "read < 8 -> EINVAL");
	}
	v = 1;
	ok(write(fd, &v, 8) == -1 && errno == EINVAL, "write -> EINVAL");

	t = its(50, 0);
	t0 = now_ms();
	ok(timerfd_settime(fd, 0, &t, NULL) == 0, "arm one-shot 50 ms");
	ok(read(fd, &v, 8) == -1 && errno == EAGAIN, "read before expiry -> EAGAIN");
	r = pollin(fd, 2000);
	t0 = now_ms() - t0;
	ok(r == 1 && t0 >= 40, "poll wakes at expiry (%lld ms)", (long long)t0);
	ok(read(fd, &v, 8) == 8 && v == 1, "one expiration");
	ok(read(fd, &v, 8) == -1 && errno == EAGAIN && pollin(fd, 0) == 0,
	    "drained after read");
	ok(timerfd_gettime(fd, &old) == 0 && old.it_value.tv_sec == 0 &&
		old.it_value.tv_nsec == 0,
	    "expired one-shot reports a zero it_value");

	t = its(10, 10);
	timerfd_settime(fd, 0, &t, NULL);
	sleep_ms(105);
	v = 0;
	r = (int)read(fd, &v, 8);
	ok(r == 8 && v >= 8 && v <= 20,
	    "periodic 10 ms: %llu expirations after 105 ms",
	    (unsigned long long)v);

	t = abs_its(50, 0);
	t0 = now_ms();
	ok(timerfd_settime(fd, TFD_TIMER_ABSTIME, &t, NULL) == 0,
	    "arm absolute now+50 ms");
	ok(pollin(fd, 2000) == 1 && now_ms() - t0 >= 40 &&
		read(fd, &v, 8) == 8 && v == 1,
	    "absolute timer fires once after ~50 ms");

	t = abs_its(-1000, 0);
	ok(timerfd_settime(fd, TFD_TIMER_ABSTIME, &t, NULL) == 0 &&
		pollin(fd, 100) == 1 && read(fd, &v, 8) == 8 && v == 1,
	    "absolute time in the past fires immediately");
	t = abs_its(-1000, 100);
	timerfd_settime(fd, TFD_TIMER_ABSTIME, &t, NULL);
	v = 0;
	r = pollin(fd, 100) == 1 && read(fd, &v, 8) == 8;
	ok(r && v >= 10 && v <= 12,
	    "periodic absolute 1 s in the past: %llu overruns",
	    (unsigned long long)v);

	t = its(1000, 500);
	timerfd_settime(fd, 0, &t, NULL);
	r = timerfd_gettime(fd, &old);
	ok(r == 0 && ts_ms(old.it_value) > 900 &&
		ts_ms(old.it_value) <= 1000 && ts_ms(old.it_interval) == 500,
	    "gettime: remaining %lld ms, interval 500 ms",
	    (long long)ts_ms(old.it_value));
	t = its(2000, 0);
	ok(timerfd_settime(fd, 0, &t, &old) == 0 && ts_ms(old.it_value) > 900 &&
		ts_ms(old.it_value) <= 1000 && ts_ms(old.it_interval) == 500,
	    "settime returns the previous setting");

	t = its(30, 0);
	timerfd_settime(fd, 0, &t, NULL);
	t = its(0, 0);
	ok(timerfd_settime(fd, 0, &t, NULL) == 0, "disarm with zero it_value");
	sleep_ms(60);
	ok(pollin(fd, 0) == 0 && read(fd, &v, 8) == -1 && errno == EAGAIN,
	    "disarmed timer never fires");
	ok(timerfd_gettime(fd, &old) == 0 && ts_ms(old.it_value) == 0,
	    "disarmed timer reports zero it_value");

	t = its(10, 0);
	timerfd_settime(fd, 0, &t, NULL);
	sleep_ms(40);
	ok(pollin(fd, 0) == 1, "short timer expired");
	t = its(1000, 0);
	timerfd_settime(fd, 0, &t, NULL);
	ok(pollin(fd, 0) == 0 && read(fd, &v, 8) == -1 && errno == EAGAIN,
	    "re-arming resets the expiration count");
	close(fd);

	fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
	t = its(50, 0);
	timerfd_settime(fd, 0, &t, NULL);
	ok(pollin(fd, 2000) == 1 && read(fd, &v, 8) == 8 && v == 1,
	    "relative CLOCK_REALTIME timer fires");
	close(fd);

	fd = timerfd_create(CLOCK_MONOTONIC, 0);
	ok(!(fcntl(fd, F_GETFL) & O_NONBLOCK) &&
		((fcntl)(fd, F_GETFD) & FD_CLOEXEC) == 0,
	    "blocking, no FD_CLOEXEC by default");
	t = its(50, 0);
	timerfd_settime(fd, 0, &t, NULL);
	t0 = now_ms();
	r = (int)read(fd, &v, 8);
	t0 = now_ms() - t0;
	ok(r == 8 && v == 1 && t0 >= 40 && t0 < 1000,
	    "blocking read returns after expiry (%lld ms)", (long long)t0);

	{
		int ep = epoll_create1(0);
		struct epoll_event ev = { EPOLLIN, { .u64 = 77 } }, out[2];
		ok(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0, "timerfd in epoll");
		t = its(30, 0);
		timerfd_settime(fd, 0, &t, NULL);
		t0 = now_ms();
		r = epoll_wait(ep, out, 2, 2000);
		t0 = now_ms() - t0;
		ok(r == 1 && out[0].data.u64 == 77 && (out[0].events & EPOLLIN) &&
			t0 >= 20,
		    "epoll_wait reports the timer (%lld ms)", (long long)t0);
		ok(read(fd, &v, 8) == 8 && v == 1 && epoll_wait(ep, out, 2, 50) == 0,
		    "read clears epoll readiness");
		close(ep);
	}
	close(fd);

	{
		int p[2];
		if (pipe(p))
			abort();
		t = its(10, 0);
		ok(timerfd_settime(p[0], 0, &t, NULL) == -1 && errno == EINVAL,
		    "settime on a pipe -> EINVAL");
		ok(timerfd_gettime(p[0], &old) == -1 && errno == EINVAL,
		    "gettime on a pipe -> EINVAL");
		close(p[0]);
		close(p[1]);
		ok(timerfd_settime(p[0], 0, &t, NULL) == -1 && errno == EBADF,
		    "settime on a closed fd -> EBADF");
	}
	return done();
}
