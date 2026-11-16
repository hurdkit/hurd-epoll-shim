/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
#define _GNU_SOURCE
#include <sys/eventfd.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tap.h"

static int late_fd;

static void *late_writer(void *arg)
{
	uint64_t v = 5;
	(void)arg;
	sleep_ms(30);
	if (write(late_fd, &v, 8) != 8)
		abort();
	return NULL;
}

struct blocked {
	int fd;
	atomic_int done;
	ssize_t r;
};

static void *overflow_writer(void *arg)
{
	struct blocked *b = arg;
	uint64_t v = 1;
	b->r = write(b->fd, &v, 8);
	atomic_store(&b->done, 1);
	return NULL;
}

static void *bulk_writer(void *arg)
{
	int fd = *(int *)arg;
	for (int i = 0; i < 10000; i++) {
		uint64_t one = 1;
		if (write(fd, &one, 8) != 8)
			abort();
	}
	return NULL;
}

int main(void)
{
	uint64_t v;
	char small[4] = { 0 };
	int fd, fl;
	pthread_t th;
	int64_t t0;

	plan(46);

	errno = 0;
	ok(eventfd(0, 2) == -1 && errno == EINVAL, "unknown flag -> EINVAL");

	fd = eventfd(0, EFD_NONBLOCK);
	ok(fd >= 0, "eventfd(0, EFD_NONBLOCK)");
	ok(read(fd, &v, 8) == -1 && errno == EAGAIN, "empty read -> EAGAIN");
	ok(pollin(fd, 0) == 0, "zero counter is not readable");
	v = 3;
	ok(write(fd, &v, 8) == 8, "write 3");
	v = 4;
	ok(write(fd, &v, 8) == 8, "write 4");
	ok(pollin(fd, 0) == 1, "non-zero counter is readable");
	v = 0;
	ok(read(fd, &v, 8) == 8 && v == 7, "read returns and clears 7");
	ok(pollin(fd, 0) == 0, "not readable after read");
	ok(read(fd, small, 4) == -1 && errno == EINVAL, "read < 8 -> EINVAL");
	ok(write(fd, small, 4) == -1 && errno == EINVAL, "write < 8 -> EINVAL");
	{
		uint64_t two[2] = { 2, 99 }, out[2] = { 0, 0 };
		ok(write(fd, two, 8) == 8 && read(fd, out, 16) == 8 &&
			out[0] == 2,
		    "a 16-byte read buffer is accepted and gets 8 bytes");
		ok(write(fd, two, 16) == -1 && errno == EINVAL,
		    "write of 16 bytes -> EINVAL (Linux >= 6.4: exactly 8)");
	}
	v = UINT64_MAX;
	ok(write(fd, &v, 8) == -1 && errno == EINVAL,
	    "write 0xffffffffffffffff -> EINVAL");
	v = UINT64_MAX - 1;
	ok(write(fd, &v, 8) == 8, "write the maximum counter value");
	v = 1;
	ok(write(fd, &v, 8) == -1 && errno == EAGAIN,
	    "nonblocking overflow -> EAGAIN");
	ok(read(fd, &v, 8) == 8 && v == UINT64_MAX - 1, "read back maximum");
	v = 0;
	ok(write(fd, &v, 8) == 8 && pollin(fd, 0) == 0,
	    "write(0) leaves an empty eventfd unreadable");
	ok(close(fd) == 0, "close");
	ok(read(fd, &v, 8) == -1 && errno == EBADF, "read after close -> EBADF");

	fd = eventfd(5, EFD_NONBLOCK);
	ok(fd >= 0 && pollin(fd, 0) == 1 && read(fd, &v, 8) == 8 && v == 5,
	    "initial value 5 is readable");
	close(fd);

	fd = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
	ok(read(fd, &v, 8) == 8 && v == 1, "EFD_SEMAPHORE read returns 1");
	ok(pollin(fd, 0) == 1, "EFD_SEMAPHORE value 2 read once still readable");
	ok(read(fd, &v, 8) == 8 && v == 1, "second EFD_SEMAPHORE read returns 1");
	ok(read(fd, &v, 8) == -1 && errno == EAGAIN && pollin(fd, 0) == 0,
	    "EFD_SEMAPHORE counter drained");
	close(fd);

	fd = eventfd(0, EFD_NONBLOCK);
	{
		eventfd_t ev = 0;
		ok(eventfd_write(fd, 9) == 0 && eventfd_read(fd, &ev) == 0 &&
			ev == 9,
		    "eventfd_write/eventfd_read");
		ok(eventfd_read(fd, &ev) == -1 && errno == EAGAIN,
		    "eventfd_read on empty -> EAGAIN");
	}
	ok(((fcntl)(fd, F_GETFD) & FD_CLOEXEC) == 0,
	    "no FD_CLOEXEC without EFD_CLOEXEC");
	ok(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
		(fcntl)(fd, F_GETFD) == FD_CLOEXEC,
	    "F_SETFD FD_CLOEXEC is visible on the real descriptor");
	fl = fcntl(fd, F_GETFL);
	ok(fl >= 0 && (fl & O_NONBLOCK) && (fl & O_ACCMODE) == O_RDWR,
	    "F_GETFL reports O_RDWR|O_NONBLOCK");
	close(fd);

	fd = eventfd(0, EFD_CLOEXEC);
	ok(fd >= 0 && ((fcntl)(fd, F_GETFD) & FD_CLOEXEC),
	    "EFD_CLOEXEC sets FD_CLOEXEC on the real descriptor");
	ok(!(fcntl(fd, F_GETFL) & O_NONBLOCK), "blocking eventfd: no O_NONBLOCK");
	ok(fcntl(fd, F_SETFL, O_NONBLOCK) == 0 &&
		(fcntl(fd, F_GETFL) & O_NONBLOCK) && read(fd, &v, 8) == -1 &&
		errno == EAGAIN,
	    "F_SETFL O_NONBLOCK makes reads nonblocking");
	ok(fcntl(fd, F_SETFL, 0) == 0 && !(fcntl(fd, F_GETFL) & O_NONBLOCK),
	    "F_SETFL 0 clears O_NONBLOCK");

	late_fd = fd;
	t0 = now_ms();
	pthread_create(&th, NULL, late_writer, NULL);
	ok(read(fd, &v, 8) == 8 && v == 5 && now_ms() - t0 >= 20,
	    "blocking read waits for a writer thread");
	pthread_join(th, NULL);

	{
		struct blocked b = { fd, 0, 0 };
		v = UINT64_MAX - 1;
		if (write(fd, &v, 8) != 8)
			abort();
		pthread_create(&th, NULL, overflow_writer, &b);
		sleep_ms(50);
		ok(!atomic_load(&b.done), "writer blocks on overflow");
		ok(read(fd, &v, 8) == 8 && v == UINT64_MAX - 1,
		    "read drains the counter");
		pthread_join(th, NULL);
		ok(b.r == 8 && read(fd, &v, 8) == 8 && v == 1,
		    "blocked writer is woken by the read");
	}

	{
		pthread_t w[4];
		uint64_t sum = 0;
		for (int i = 0; i < 4; i++)
			pthread_create(&w[i], NULL, bulk_writer, &fd);
		while (sum < 40000 && read(fd, &v, 8) == 8)
			sum += v;
		for (int i = 0; i < 4; i++)
			pthread_join(w[i], NULL);
		ok(sum == 40000, "4 concurrent writers: no lost increments");
	}
	close(fd);

	{
		int p[2], r;
		struct pollfd pf[2];
		if (pipe(p) || write(p[1], "x", 1) != 1)
			abort();
		fd = eventfd(0, EFD_NONBLOCK);
		pf[0] = (struct pollfd){ p[0], POLLIN, 0 };
		pf[1] = (struct pollfd){ fd, POLLIN, 0 };
		t0 = now_ms();
		r = poll(pf, 2, 1000);
		ok(r == 1 && (pf[0].revents & POLLIN) && pf[1].revents == 0 &&
			now_ms() - t0 < 100,
		    "raw poll: readable pipe + empty eventfd returns 1 at once");
		close(p[0]);
		close(p[1]);
	}

	{
		int d = fcntl(fd, F_DUPFD_CLOEXEC, 0);
		ok(d >= 0 && d != fd && ((fcntl)(d, F_GETFD) & FD_CLOEXEC),
		    "F_DUPFD_CLOEXEC duplicates an eventfd");
		v = 6;
		ok(write(fd, &v, 8) == 8 && pollin(d, 0) == 1 &&
			read(d, &v, 8) == 8 && v == 6,
		    "duplicate shares the counter");
		close(fd);
		v = 2;
		ok(write(d, &v, 8) == 8 && pollin(d, 0) == 1 &&
			read(d, &v, 8) == 8 && v == 2,
		    "duplicate still works after the original is closed");
		ok(close(d) == 0, "close duplicate");
	}

#if SHIM
	{
		struct stat st;
		fd = eventfd(0, 0);
		ok(fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode),
		    "shim is active: eventfd is a pipe");
		ok(epoll_shim_version() != NULL, "epoll_shim_version: %s",
		    epoll_shim_version());
		close(fd);
	}
#else
	skip(2, "kernel run: shim-only check");
#endif
	return done();
}
