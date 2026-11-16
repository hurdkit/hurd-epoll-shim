/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Interposition of read/write/close/fcntl on plain fds, shim proof, fork. */
#define _GNU_SOURCE
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tap.h"

static inline int is_fifo(int fd)
{
	struct stat st;
	return fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode);
}

static void *waiter(void *arg)
{
	struct epoll_event ev;
	epoll_wait(*(int *)arg, &ev, 1, 300);
	return NULL;
}

/* Child: 0 on success, otherwise the number of the failed check. */
static int child(int efd, int ep)
{
	uint64_t v = 1;
	int fd;
	alarm(10);
#if SHIM
	if (read(efd, &v, 8) != -1 || errno != EBADF)
		return 1; /* documented: inherited shim fds only support close */
#else
	if (fcntl(efd, F_GETFL) < 0)
		return 1;
#endif
	if (close(efd) != 0 || close(ep) != 0)
		return 2;
	fd = eventfd(3, EFD_NONBLOCK);
	if (fd < 0 || read(fd, &v, 8) != 8 || v != 3)
		return 3;
	if (close(fd) != 0)
		return 4;
	return 0;
}

int main(int argc, char **argv)
{
	int p[2], efd, ep, tfd, st, r;
	char c = 0;
	uint64_t v;
	pid_t pid;

	(void)argc;
	plan(15);

	efd = eventfd(0, EFD_NONBLOCK);
	ep = epoll_create1(0);
	tfd = timerfd_create(CLOCK_MONOTONIC, 0);
#if SHIM
	ok(epoll_shim_version() != NULL, "epoll_shim_version() = %s",
	    epoll_shim_version());
	ok(is_fifo(efd) && is_fifo(ep) && is_fifo(tfd),
	    "the shim, not the kernel, implements eventfd/epoll/timerfd");
#else
	skip(2, "kernel run: shim-only check");
#endif
	close(tfd);

	if (pipe(p))
		abort();
	ok(write(p[1], "q", 1) == 1 && read(p[0], &c, 1) == 1 && c == 'q',
	    "read/write forwarded for a plain pipe");
	ok(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0 &&
		(fcntl(p[0], F_GETFL) & O_NONBLOCK) &&
		((fcntl)(p[0], F_GETFL) & O_NONBLOCK),
	    "fcntl forwarded for a plain pipe");
	ok(read(p[0], &c, 1) == -1 && errno == EAGAIN, "plain fd semantics kept");
	{
		int d = fcntl(p[1], F_DUPFD, 100);
		ok(d >= 100 && write(d, "z", 1) == 1 && close(d) == 0,
		    "F_DUPFD with a minimum forwarded for plain fds");
	}
	ok(close(p[0]) == 0 && close(p[1]) == 0, "close forwarded");
	ok(close(p[0]) == -1 && errno == EBADF, "close of a closed fd -> EBADF");
	ok(write(p[1], "x", 1) == -1 && errno == EBADF,
	    "write to a closed fd -> EBADF");

	{
		struct flock fl;
		int f = open(argv[0], O_RDONLY);
		memset(&fl, 0, sizeof fl);
		fl.l_type = F_RDLCK;
		fl.l_whence = SEEK_SET;
		r = f >= 0 && fcntl(f, F_SETLK, &fl) == 0;
		fl.l_type = F_UNLCK;
		ok(r && fcntl(f, F_SETLK, &fl) == 0,
		    "pointer-argument fcntl (F_SETLK) is forwarded");
		close(f);
	}

	/* fork while another thread sits in epoll_wait */
	{
		pthread_t th;
		struct epoll_event ev = { EPOLLIN, { .u64 = 1 } };
		epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
		pthread_create(&th, NULL, waiter, &ep);
		sleep_ms(50);
		pid = fork();
		if (pid == 0)
			_exit(child(efd, ep));
		ok(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
			WEXITSTATUS(st) == 0,
		    "child: inherited fds close, new eventfd works (status %d)",
		    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		pthread_join(th, NULL);
	}
	v = 4;
	ok(write(efd, &v, 8) == 8, "parent eventfd still works after fork");
	{
		struct epoll_event ev;
		ok(epoll_wait(ep, &ev, 1, 1000) == 1 && ev.data.u64 == 1,
		    "parent epoll still works after fork");
	}
	ok(read(efd, &v, 8) == 8 && v == 4, "parent reads its value");
	ok(close(efd) == 0 && close(ep) == 0, "close");
	return done();
}
