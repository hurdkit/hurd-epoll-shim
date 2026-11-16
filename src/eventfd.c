/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* eventfd: a 64-bit counter, readable while non-zero. */
#include "shim.h"

API int eventfd(unsigned int initval, int flags)
{
	struct obj *o;

	if (flags & ~(EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC))
		return fail(EINVAL);
	if (!(o = obj_new(K_EVENTFD, flags & EFD_NONBLOCK)))
		return -1;
	o->u.ev.count = initval;
	o->u.ev.sem = flags & EFD_SEMAPHORE;
	set_readable(o, initval > 0);
	return obj_publish(o, flags & EFD_CLOEXEC);
}

/* Reads accept buffers of 8 bytes or more; writes need exactly 8 bytes,
 * as on Linux since 6.4. */
ssize_t eventfd_rw(struct obj *o, void *buf, size_t n, bool wr)
{
	uint64_t v = 0, *c = &o->u.ev.count;
	ssize_t r = sizeof v;

	if (wr ? n != sizeof v : n < sizeof v)
		return fail(EINVAL);
	if (wr) {
		memcpy(&v, buf, sizeof v);
		if (v == UINT64_MAX)
			return fail(EINVAL);
	}
	pthread_mutex_lock(&o->mu);
	for (;;) {
		if (o->closing) {
			r = fail(EBADF);
		} else if (wr && UINT64_MAX - 1 - *c >= v) {
			*c += v;
		} else if (wr && o->nonblock) {
			r = fail(EAGAIN);
		} else if (wr) {
			pthread_cond_wait(&o->cv, &o->mu); /* woken by reads */
			continue;
		} else if (*c > 0) {
			v = o->u.ev.sem ? 1 : *c;
			*c -= v;
			pthread_cond_broadcast(&o->cv);
		} else if (block_on(o) < 0) {
			r = -1;
		} else {
			continue;
		}
		break;
	}
	set_readable(o, *c > 0);
	pthread_mutex_unlock(&o->mu);
	if (r > 0 && !wr)
		memcpy(buf, &v, sizeof v);
	return r;
}

API int eventfd_read(int fd, eventfd_t *value)
{
	return epoll_shim_read(fd, value, sizeof *value) == (ssize_t)sizeof *value ? 0 : -1;
}

API int eventfd_write(int fd, eventfd_t value)
{
	return epoll_shim_write(fd, &value, sizeof value) == (ssize_t)sizeof value ? 0 : -1;
}
