/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include "tap.h"

static int add(int ep, int fd, uint32_t events, uint64_t data)
{
	struct epoll_event ev;
	ev.events = events;
	ev.data.u64 = data;
	return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

static int mod(int ep, int fd, uint32_t events, uint64_t data)
{
	struct epoll_event ev;
	ev.events = events;
	ev.data.u64 = data;
	return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
}

static void mkpipe(int p[2])
{
	if (pipe(p))
		abort();
}

static void put(int fd)
{
	if (write(fd, "x", 1) != 1)
		abort();
}

static void drain(int fd)
{
	char buf[64];
	int fl = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	while (read(fd, buf, sizeof buf) > 0)
		;
	fcntl(fd, F_SETFL, fl);
}

struct delayed {
	int fd, ms;
};

static void *delayed_put(void *arg)
{
	struct delayed *d = arg;
	sleep_ms(d->ms);
	put(d->fd);
	return NULL;
}

struct waiter {
	int ep, timeout, r;
	uint64_t data;
	int64_t ms;
};

static void *wait_thread(void *arg)
{
	struct waiter *w = arg;
	struct epoll_event ev[4];
	int64_t t0 = now_ms();
	w->r = epoll_wait(w->ep, ev, 4, w->timeout);
	w->ms = now_ms() - t0;
	if (w->r > 0)
		w->data = ev[0].data.u64;
	return NULL;
}

static atomic_int hits;
static void on_usr1(int s)
{
	(void)s;
	atomic_fetch_add(&hits, 1);
}

static pthread_t main_thread;
static void *kick_main(void *arg)
{
	(void)arg;
	sleep_ms(50);
	pthread_kill(main_thread, SIGUSR1);
	return NULL;
}

struct churn {
	int ep, fd;
	atomic_int *stop;
};

static void *ctl_churn(void *arg)
{
	struct churn *c = arg;
	while (!atomic_load(c->stop)) {
		add(c->ep, c->fd, EPOLLIN, 1);
		epoll_ctl(c->ep, EPOLL_CTL_DEL, c->fd, NULL);
	}
	return NULL;
}

int main(void)
{
	struct epoll_event out[8];
	int ep, p[2], q[2], r;
	int64_t t0;

	volatile int minus_one = -1; /* hide it from glibc's access attribute */

	main_thread = pthread_self();
	plan(67);

	errno = 0;
	ok(epoll_create(0) == -1 && errno == EINVAL, "epoll_create(0) -> EINVAL");
	ok(epoll_create1(1) == -1 && errno == EINVAL,
	    "epoll_create1(unknown flag) -> EINVAL");
	ep = epoll_create1(EPOLL_CLOEXEC);
	ok(ep >= 0 && ((fcntl)(ep, F_GETFD) & FD_CLOEXEC),
	    "EPOLL_CLOEXEC sets FD_CLOEXEC on the real fd");
	close(ep);
	ep = epoll_create(1);
	ok(ep >= 0 && ((fcntl)(ep, F_GETFD) & FD_CLOEXEC) == 0,
	    "epoll_create(1), no FD_CLOEXEC");
	mkpipe(p);

	ok(add(ep, 9999, EPOLLIN, 0) == -1 && errno == EBADF, "ADD bad fd -> EBADF");
	ok(add(p[0], p[1], EPOLLIN, 0) == -1 && errno == EINVAL,
	    "epfd not an epoll fd -> EINVAL");
	ok(add(9998, p[0], EPOLLIN, 0) == -1 && errno == EBADF, "bad epfd -> EBADF");
	ok(add(ep, ep, EPOLLIN, 0) == -1 && errno == EINVAL, "fd == epfd -> EINVAL");
	ok(add(ep, p[0], EPOLLIN, 1) == 0, "ADD pipe read end");
	ok(add(ep, p[0], EPOLLIN, 1) == -1 && errno == EEXIST, "ADD twice -> EEXIST");
	ok(mod(ep, p[1], EPOLLOUT, 0) == -1 && errno == ENOENT,
	    "MOD unregistered -> ENOENT");
	ok(epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL) == -1 && errno == ENOENT,
	    "DEL unregistered -> ENOENT");
	{
		struct epoll_event ev = { EPOLLIN, { 0 } };
		ok(epoll_ctl(ep, 99, p[0], &ev) == -1 && errno == EINVAL,
		    "unknown op -> EINVAL");
	}
	ok(epoll_wait(ep, out, 0, 0) == -1 && errno == EINVAL,
	    "maxevents 0 -> EINVAL");
	ok(epoll_wait(ep, out, minus_one, 0) == -1 && errno == EINVAL,
	    "maxevents -1 -> EINVAL");
	ok(epoll_wait(p[0], out, 8, 0) == -1 && errno == EINVAL,
	    "epoll_wait on a pipe -> EINVAL");
	ok(epoll_wait(9998, out, 8, 0) == -1 && errno == EBADF,
	    "epoll_wait on a bad fd -> EBADF");

	t0 = now_ms();
	ok(epoll_wait(ep, out, 8, 0) == 0 && now_ms() - t0 < 50,
	    "timeout 0 returns 0 at once");
	t0 = now_ms();
	r = epoll_wait(ep, out, 8, 100);
	t0 = now_ms() - t0;
	ok(r == 0 && t0 >= 90, "timeout 100 returns 0 after %lld ms",
	    (long long)t0);
	ok(pollin(ep, 0) == 0, "raw poll: idle epoll fd is not readable");

	put(p[1]);
	ok(mod(ep, p[0], EPOLLIN, 0x1122334455667788ull) == 0, "MOD data");
	r = epoll_wait(ep, out, 8, 1000);
	ok(r == 1 && out[0].events == EPOLLIN &&
		out[0].data.u64 == 0x1122334455667788ull,
	    "EPOLLIN with the 64-bit data");
	ok(pollin(ep, 1000) == 1, "raw poll: epoll fd with a ready pipe is readable");
	drain(p[0]);
	ok(epoll_wait(ep, out, 8, 0) == 0, "consumed data is no longer reported");
	ok(pollin(ep, 100) == 0, "raw poll: epoll fd readability cleared");

	ok(add(ep, p[1], EPOLLOUT, 2) == 0 && epoll_wait(ep, out, 8, 1000) == 1 &&
		out[0].events == EPOLLOUT && out[0].data.u64 == 2,
	    "EPOLLOUT on the write end");
	ok(epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL) == 0 &&
		epoll_wait(ep, out, 8, 50) == 0,
	    "DEL removes the entry");

	put(p[1]);
	ok(mod(ep, p[0], EPOLLPRI, 3) == 0 && epoll_wait(ep, out, 8, 100) == 0,
	    "EPOLLPRI-only interest on a readable pipe reports nothing");
	ok(mod(ep, p[0], EPOLLIN | EPOLLONESHOT, 4) == 0 &&
		epoll_wait(ep, out, 8, 1000) == 1 && out[0].data.u64 == 4,
	    "EPOLLONESHOT reports once");
	ok(epoll_wait(ep, out, 8, 50) == 0, "then is disarmed");
	ok(mod(ep, p[0], EPOLLIN | EPOLLONESHOT, 5) == 0 &&
		epoll_wait(ep, out, 8, 1000) == 1 && out[0].data.u64 == 5,
	    "MOD re-arms a oneshot entry");
	ok(mod(ep, p[0], EPOLLIN | EPOLLET, 6) == 0 &&
		epoll_wait(ep, out, 8, 1000) == 1 && out[0].data.u64 == 6,
	    "EPOLLET is accepted");

	/* DEL of the only ready fd clears the epoll fd's readability. */
	mod(ep, p[0], EPOLLIN, 6);
	ok(pollin(ep, 1000) == 1, "epoll fd readable with a ready entry");
	epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL);
	ok(pollin(ep, 100) == 0, "DEL clears epoll fd readability");

	/* close() without EPOLL_CTL_DEL drops the registration */
	ok(add(ep, p[0], EPOLLIN, 7) == 0 && epoll_wait(ep, out, 8, 1000) == 1,
	    "re-added ready pipe");
	close(p[0]);
	close(p[1]);
	ok(epoll_wait(ep, out, 8, 100) == 0,
	    "close() without DEL removes it from the set");
	mkpipe(q);
	put(q[1]);
	ok(epoll_wait(ep, out, 8, 100) == 0,
	    "a reused fd number is not reported with stale data");
	ok(add(ep, q[0], EPOLLIN, 8) == 0, "reused fd number can be added again");
	ok(epoll_wait(ep, out, 8, 1000) == 1 && out[0].data.u64 == 8,
	    "and reports its own data");
	/* close through the unwrapped libc function */
	(close)(q[0]);
	ok(epoll_wait(ep, out, 8, 100) == 0,
	    "fd closed behind the shim's back is dropped");
	close(q[1]);

	/* fairness and maxevents */
	{
		int a[2], b[2], c[2];
		uint64_t first;
		mkpipe(a);
		mkpipe(b);
		mkpipe(c);
		put(a[1]);
		put(b[1]);
		put(c[1]);
		add(ep, a[0], EPOLLIN, 10);
		add(ep, b[0], EPOLLIN, 11);
		add(ep, c[0], EPOLLIN, 12);
		ok(epoll_wait(ep, out, 2, 1000) == 2, "maxevents limits the result");
		ok(epoll_wait(ep, out, 1, 1000) == 1, "maxevents 1");
		first = out[0].data.u64;
		ok(epoll_wait(ep, out, 1, 1000) == 1 && out[0].data.u64 != first,
		    "ready entries are reported round-robin");
		close(a[0]);
		close(a[1]);
		close(b[0]);
		close(b[1]);
		close(c[0]);
		close(c[1]);
	}

	/* EPOLLRDHUP only when requested */
	{
		int sv[2];
		uint32_t e;
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
			abort();
		add(ep, sv[0], EPOLLIN, 20);
		close(sv[1]);
		r = epoll_wait(ep, out, 8, 1000);
		e = out[0].events;
		ok(r == 1 && (e & EPOLLIN) && !(e & EPOLLRDHUP),
		    "peer close without EPOLLRDHUP interest: events %#x", e);
		mod(ep, sv[0], EPOLLIN | EPOLLRDHUP, 21);
		r = epoll_wait(ep, out, 8, 1000);
		e = out[0].events;
#ifdef __linux__
		ok(r == 1 && (e & EPOLLRDHUP), "EPOLLRDHUP reported when requested");
#else
		(void)r;
		skip(1, "no POLLRDHUP/POLLHUP for sockets on this system");
#endif
		close(sv[0]);
	}
	ok(epoll_wait(ep, out, 8, 0) == 0, "set is empty again");

	/* raw poll() on the epoll fd wakes when another thread writes */
	{
		pthread_t th;
		struct delayed d;
		int t;
		mkpipe(p);
		add(ep, p[0], EPOLLIN, 30);
		d.fd = p[1];
		d.ms = 30;
		t0 = now_ms();
		pthread_create(&th, NULL, delayed_put, &d);
		t = pollin(ep, 2000);
		t0 = now_ms() - t0;
		ok(t == 1 && t0 < 100,
		    "raw poll on epoll fd wakes %lld ms after a 30 ms write",
		    (long long)t0);
		pthread_join(th, NULL);
		ok(epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 30,
		    "and epoll_wait reports the pipe");
		drain(p[0]);
	}

	/* epoll_wait blocked in a thread */
	{
		pthread_t th;
		struct waiter w = { ep, 5000, 0, 0, 0 };
		pthread_create(&th, NULL, wait_thread, &w);
		sleep_ms(30);
		put(p[1]);
		pthread_join(th, NULL);
		ok(w.r == 1 && w.data == 30 && w.ms < 1000,
		    "blocked epoll_wait woken by a write");
		drain(p[0]);
		mkpipe(q);
		put(q[1]);
		w.r = 0;
		pthread_create(&th, NULL, wait_thread, &w);
		sleep_ms(30);
		add(ep, q[0], EPOLLIN, 31);
		pthread_join(th, NULL);
		ok(w.r == 1 && w.data == 31 && w.ms < 1000,
		    "blocked epoll_wait woken by ADD of a ready fd");
		close(q[0]);
		close(q[1]);
	}

	/* nested epoll */
	{
		int outer = epoll_create1(0);
		ok(add(outer, ep, EPOLLIN, 40) == 0, "epoll fd added to another epoll");
		ok(epoll_wait(outer, out, 8, 50) == 0, "outer idle while inner idle");
		put(p[1]);
		ok(epoll_wait(outer, out, 8, 1000) == 1 && out[0].data.u64 == 40 &&
			(out[0].events & EPOLLIN),
		    "outer reports the inner epoll fd");
		ok(pollin(outer, 1000) == 1, "raw poll on outer epoll fd");
		ok(epoll_wait(ep, out, 8, 0) == 1 && out[0].data.u64 == 30,
		    "inner reports the pipe");
		drain(p[0]);
		ok(epoll_wait(ep, out, 8, 0) == 0 && epoll_wait(outer, out, 8, 100) == 0,
		    "both idle after draining");
		close(outer);
	}

	/* EFD_SEMAPHORE through epoll and raw poll */
	{
		int efd = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
		uint64_t v;
		add(ep, efd, EPOLLIN, 50);
		ok(epoll_wait(ep, out, 8, 1000) == 1 && out[0].data.u64 == 50,
		    "semaphore eventfd reported");
		ok(read(efd, &v, 8) == 8 && v == 1, "read once");
		ok(epoll_wait(ep, out, 8, 1000) == 1 && pollin(ep, 1000) == 1,
		    "value 2 read once: still readable via epoll and raw poll");
		ok(read(efd, &v, 8) == 8 && epoll_wait(ep, out, 8, 50) == 0 &&
			pollin(ep, 100) == 0,
		    "drained semaphore is idle");
		close(efd);
	}

	/* many fds */
	{
		int fds[50][2];
		for (int i = 0; i < 50; i++) {
			mkpipe(fds[i]);
			add(ep, fds[i][0], EPOLLIN, 100 + i);
		}
		put(fds[37][1]);
		ok(epoll_wait(ep, out, 8, 1000) == 1 && out[0].data.u64 == 137,
		    "one ready fd among 50");
		for (int i = 0; i < 50; i++) {
			close(fds[i][0]);
			close(fds[i][1]);
		}
	}

	/* dup of an epoll fd */
	{
		int d = fcntl(ep, F_DUPFD_CLOEXEC, 0);
		put(p[1]);
		ok(d >= 0 && epoll_wait(d, out, 8, 1000) == 1 && out[0].data.u64 == 30,
		    "F_DUPFD_CLOEXEC of an epoll fd shares the interest list");
		drain(p[0]);
		close(d);
	}

	/* epoll_pwait with a signal mask */
	{
		struct sigaction sa;
		sigset_t usr1, waitmask, cur;
		pthread_t th;
		memset(&sa, 0, sizeof sa);
		sa.sa_handler = on_usr1;
		sigaction(SIGUSR1, &sa, NULL);
		sigemptyset(&usr1);
		sigaddset(&usr1, SIGUSR1);
		pthread_sigmask(SIG_BLOCK, &usr1, NULL);
		pthread_sigmask(SIG_SETMASK, NULL, &waitmask);
		sigdelset(&waitmask, SIGUSR1);
		pthread_create(&th, NULL, kick_main, NULL);
		t0 = now_ms();
		r = epoll_pwait(ep, out, 8, 3000, &waitmask);
		ok(r == -1 && errno == EINTR && atomic_load(&hits) == 1 &&
			now_ms() - t0 < 2000,
		    "epoll_pwait: signal unblocked by sigmask interrupts with EINTR");
		pthread_join(th, NULL);
		pthread_sigmask(SIG_SETMASK, NULL, &cur);
		ok(sigismember(&cur, SIGUSR1) == 1, "epoll_pwait restored the mask");
		ok(epoll_pwait(ep, out, 8, 0, NULL) == 0, "epoll_pwait NULL sigmask");
	}

	/* concurrent ADD/DEL vs epoll_wait */
	{
		atomic_int stop = 0;
		struct churn c = { ep, p[0], &stop };
		pthread_t th;
		int bad = 0;
		epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL);
		put(p[1]);
		pthread_create(&th, NULL, ctl_churn, &c);
		for (int i = 0; i < 2000; i++) {
			r = epoll_wait(ep, out, 8, 0);
			if (r < 0 || (r == 1 && out[0].data.u64 != 1))
				bad++;
		}
		atomic_store(&stop, 1);
		pthread_join(th, NULL);
		ok(bad == 0, "concurrent epoll_ctl and epoll_wait");
	}

	close(p[0]);
	close(p[1]);
	ok(close(ep) == 0, "close epoll fd");
	return done();
}
