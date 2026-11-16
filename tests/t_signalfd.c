/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "tap.h"

#define REC sizeof(struct signalfd_siginfo)

static int mask_equal(const sigset_t *a, const sigset_t *b)
{
	for (int s = 1; s < 32; s++)
		if (sigismember(a, s) != sigismember(b, s))
			return 0;
	return 1;
}

static int blk_fd;
static uint32_t blk_signo;

static void *blocking_reader(void *arg)
{
	struct signalfd_siginfo si;
	(void)arg;
	if (read(blk_fd, &si, REC) == (ssize_t)REC)
		blk_signo = si.ssi_signo;
	return NULL;
}

static void *churn(void *arg)
{
	sigset_t m;
	(void)arg;
	sigemptyset(&m);
	sigaddset(&m, SIGUSR2);
	for (int i = 0; i < 50; i++) {
		int fd = signalfd(-1, &m, SFD_NONBLOCK);
		if (fd < 0)
			return (void *)1;
		close(fd);
	}
	return NULL;
}

int main(void)
{
	sigset_t blocked, before, after, m1, m12;
	struct signalfd_siginfo si[4];
	int fd, fd2;
	ssize_t n;

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	sigaddset(&blocked, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &blocked, NULL);
	sigemptyset(&m1);
	sigaddset(&m1, SIGUSR1);
	m12 = blocked;

	plan(30);

	ok(sizeof(struct signalfd_siginfo) == 128 &&
		offsetof(struct signalfd_siginfo, ssi_int) == 44 &&
		offsetof(struct signalfd_siginfo, ssi_ptr) == 48 &&
		offsetof(struct signalfd_siginfo, ssi_addr) == 72,
	    "struct signalfd_siginfo has the Linux layout");
	errno = 0;
	ok(signalfd(-1, &m1, 2) == -1 && errno == EINVAL,
	    "unknown flag -> EINVAL");

	pthread_sigmask(SIG_SETMASK, NULL, &before);
	fd = signalfd(-1, &m1, SFD_NONBLOCK | SFD_CLOEXEC);
	pthread_sigmask(SIG_SETMASK, NULL, &after);
	ok(fd >= 0, "signalfd(-1, {SIGUSR1}, SFD_NONBLOCK|SFD_CLOEXEC)");
	ok(mask_equal(&before, &after), "caller's signal mask is unchanged");
	ok(((fcntl)(fd, F_GETFD) & FD_CLOEXEC) && (fcntl(fd, F_GETFL) & O_NONBLOCK),
	    "FD_CLOEXEC on the real fd, O_NONBLOCK in F_GETFL");
	ok(read(fd, si, REC) == -1 && errno == EAGAIN, "empty read -> EAGAIN");
	ok(read(fd, si, REC - 1) == -1 && errno == EINVAL, "read < 128 -> EINVAL");
	ok(pollin(fd, 0) == 0, "not readable before a signal");

	kill(getpid(), SIGUSR1);
	ok(pollin(fd, 2000) == 1, "readable after kill(getpid(), SIGUSR1)");
	n = read(fd, si, sizeof si);
	ok(n == (ssize_t)REC && si[0].ssi_signo == SIGUSR1 &&
		si[0].ssi_code == SI_USER,
	    "one record: SIGUSR1, SI_USER");
#ifdef __GNU__
	skip(1, "Hurd does not report the sender pid/uid");
#else
	ok(si[0].ssi_pid == (uint32_t)getpid() && si[0].ssi_uid == getuid(),
	    "ssi_pid and ssi_uid are filled");
#endif
	ok(pollin(fd, 0) == 0 && read(fd, si, REC) == -1 && errno == EAGAIN,
	    "drained after read");

	ok(signalfd(fd, &m12, 0) == fd, "update mask to {SIGUSR1, SIGUSR2}");
	sleep_ms(200); /* let the shim pick up the new mask */
	kill(getpid(), SIGUSR1);
	kill(getpid(), SIGUSR2);
	ok(pollin(fd, 2000) == 1 && read(fd, si, REC) == (ssize_t)REC,
	    "two signals queued: one record read");
	{
		uint32_t first = si[0].ssi_signo;
		ok(pollin(fd, 2000) == 1, "still readable after reading one record");
		ok(read(fd, si, REC) == (ssize_t)REC &&
			first + si[0].ssi_signo == SIGUSR1 + SIGUSR2,
		    "second record is the other signal");
	}
	ok(read(fd, si, REC) == -1 && errno == EAGAIN, "both consumed");

	kill(getpid(), SIGUSR1);
	kill(getpid(), SIGUSR2);
	sleep_ms(300);
	n = read(fd, si, sizeof si);
	ok(n == 2 * (ssize_t)REC &&
		si[0].ssi_signo + si[1].ssi_signo == SIGUSR1 + SIGUSR2,
	    "a large buffer returns several records per read");

	{
		int ep = epoll_create1(0);
		struct epoll_event ev = { EPOLLIN, { .u64 = 5 } }, out[2];
		epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
		ok(epoll_wait(ep, out, 2, 0) == 0, "signalfd idle in epoll");
		kill(getpid(), SIGUSR2);
		ok(epoll_wait(ep, out, 2, 2000) == 1 && out[0].data.u64 == 5 &&
			(out[0].events & EPOLLIN),
		    "epoll reports a pending signal");
		ok(read(fd, si, REC) == (ssize_t)REC && si[0].ssi_signo == SIGUSR2 &&
			epoll_wait(ep, out, 2, 50) == 0,
		    "reading clears epoll readiness");
		close(ep);
	}
	ok(close(fd) == 0, "close");

	/* Signal pending before the signalfd exists: reported exactly once
	 * (catches sigwait implementations that never dequeue it). */
	sleep_ms(200);
	kill(getpid(), SIGUSR2);
	sigemptyset(&m1);
	sigaddset(&m1, SIGUSR2);
	fd2 = signalfd(-1, &m1, SFD_NONBLOCK);
	ok(pollin(fd2, 2000) == 1 && read(fd2, si, sizeof si) == (ssize_t)REC &&
		si[0].ssi_signo == SIGUSR2,
	    "signal pending before signalfd() is reported once");
	sleep_ms(300);
	ok(read(fd2, si, sizeof si) == -1 && errno == EAGAIN,
	    "and is not reported again");
	close(fd2);

	{
		pthread_t th;
		sigemptyset(&m1);
		sigaddset(&m1, SIGUSR1);
		blk_fd = signalfd(-1, &m1, 0);
		pthread_create(&th, NULL, blocking_reader, NULL);
		sleep_ms(200);
		kill(getpid(), SIGUSR1);
		pthread_join(th, NULL);
		ok(blk_signo == SIGUSR1, "blocking read in a thread is woken");
		close(blk_fd);
	}

	{
		int p[2];
		if (pipe(p))
			abort();
		ok(signalfd(p[0], &m1, 0) == -1 && errno == EINVAL,
		    "signalfd(pipe) -> EINVAL");
		close(p[0]);
		close(p[1]);
		ok(signalfd(p[0], &m1, 0) == -1 && errno == EBADF,
		    "signalfd(closed fd) -> EBADF");
	}

	{
		pthread_t a, b;
		void *ra, *rb;
		pthread_create(&a, NULL, churn, NULL);
		pthread_create(&b, NULL, churn, NULL);
		pthread_join(a, &ra);
		pthread_join(b, &rb);
		ok(ra == NULL && rb == NULL, "concurrent signalfd create/close");
	}

	pthread_sigmask(SIG_SETMASK, NULL, &after);
	ok(sigismember(&after, SIGUSR1) && sigismember(&after, SIGUSR2) &&
		!sigismember(&after, SIGTERM),
	    "caller's mask still blocks exactly what it blocked");
	{
		/* After the last signalfd is gone the default disposition must
		 * be back in place (the shim restores what it replaced). */
		struct sigaction sa;
		sleep_ms(200);
		ok(sigaction(SIGUSR1, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL,
		    "SIGUSR1 disposition restored after the last close");
	}
	return done();
}
