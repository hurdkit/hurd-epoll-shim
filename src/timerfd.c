/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* timerfd: one helper thread per armed timer sleeps on the object's
 * condition variable (CLOCK_MONOTONIC) until the next expiration. */
#include "shim.h"

#define NS INT64_C(1000000000)
#define NS_MAX (INT64_C(1) << 62) /* ~146 years; keeps sums below 2^63 */

static int64_t ts_ns(const struct timespec *ts)
{
	return ts->tv_sec >= NS_MAX / NS ? NS_MAX : ts->tv_sec * NS + ts->tv_nsec;
}

static struct timespec ns_ts(int64_t ns)
{
	struct timespec ts = { (time_t)(ns / NS), (long)(ns % NS) };
	return ts;
}

static bool ts_valid(const struct timespec *ts)
{
	return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < NS;
}

/* o->mu held: account for expirations up to now. */
static void tick(struct obj *o, int64_t now)
{
	if (!o->u.tm.armed || now < o->u.tm.next)
		return;
	if (o->u.tm.interval > 0) {
		int64_t k = (now - o->u.tm.next) / o->u.tm.interval + 1;
		o->u.tm.ticks += (uint64_t)k;
		o->u.tm.next += k * o->u.tm.interval;
	} else {
		o->u.tm.ticks++;
		o->u.tm.armed = false;
	}
	set_readable(o, true);
}

/* Account for an expiration that is due but that the helper thread has not
 * published yet, so that a wait which starts after the deadline sees the
 * timer as the kernel would show it. */
void timerfd_sync(struct obj *o)
{
	pthread_mutex_lock(&o->mu);
	if (!o->closing)
		tick(o, now_ns(o->u.tm.clock));
	pthread_mutex_unlock(&o->mu);
}

static void *timer_main(void *arg)
{
	struct obj *o = arg;

	pthread_mutex_lock(&o->mu);
	while (!o->closing) {
		int64_t now = now_ns(o->u.tm.clock);
		struct timespec d;
		tick(o, now);
		/* Once readable, later periods are counted lazily by tick() in
		 * read/gettime; sleep until one of those or settime. */
		if (!o->u.tm.armed || o->u.tm.ticks > 0) {
			pthread_cond_wait(&o->cv, &o->mu);
			continue;
		}
		d = cond_deadline(o->u.tm.next - now);
		pthread_cond_timedwait(&o->cv, &o->mu, &d);
	}
	pthread_mutex_unlock(&o->mu);
	return NULL;
}

API int timerfd_create(clockid_t clockid, int flags)
{
	struct obj *o;

#ifdef CLOCK_BOOTTIME
	if (clockid == CLOCK_BOOTTIME)
		clockid = CLOCK_MONOTONIC;
#endif
	if (clockid != CLOCK_MONOTONIC && clockid != CLOCK_REALTIME)
		return fail(EINVAL);
	if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC))
		return fail(EINVAL);
	if (!(o = obj_new(K_TIMERFD, flags & TFD_NONBLOCK)))
		return -1;
	o->u.tm.clock = clockid;
	return obj_publish(o, flags & TFD_CLOEXEC);
}

static void get_locked(struct obj *o, int64_t now, struct itimerspec *cur)
{
	tick(o, now);
	cur->it_value = ns_ts(o->u.tm.armed ? o->u.tm.next - now : 0);
	cur->it_interval = ns_ts(o->u.tm.interval);
}

API int timerfd_settime(int fd, int flags, const struct itimerspec *nv,
    struct itimerspec *old)
{
	struct obj *o;
	int64_t now;
	int r = 0;

	if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))
		return fail(EINVAL);
	if (!nv)
		return fail(EFAULT);
	if (!ts_valid(&nv->it_value) || !ts_valid(&nv->it_interval))
		return fail(EINVAL);
	if (!(o = obj_get_kind(fd, K_TIMERFD)))
		return -1;
	pthread_mutex_lock(&o->mu);
	now = now_ns(o->u.tm.clock);
	if (o->closing) {
		r = fail(EBADF);
		goto out;
	}
	if (old)
		get_locked(o, now, old);
	o->u.tm.ticks = 0;
	set_readable(o, false);
	o->u.tm.interval = ts_ns(&nv->it_interval);
	o->u.tm.armed = nv->it_value.tv_sec != 0 || nv->it_value.tv_nsec != 0;
	o->u.tm.next = ts_ns(&nv->it_value);
	if (!(flags & TFD_TIMER_ABSTIME))
		o->u.tm.next += now;
	tick(o, now);
	if (o->u.tm.armed && !o->has_thread) {
		if ((r = start_thread(&o->thread, timer_main, o)) == 0)
			o->has_thread = true;
	}
	pthread_cond_broadcast(&o->cv);
out:
	pthread_mutex_unlock(&o->mu);
	obj_put(o);
	return r;
}

API int timerfd_gettime(int fd, struct itimerspec *cur)
{
	struct obj *o;

	if (!cur)
		return fail(EFAULT);
	if (!(o = obj_get_kind(fd, K_TIMERFD)))
		return -1;
	pthread_mutex_lock(&o->mu);
	get_locked(o, now_ns(o->u.tm.clock), cur);
	pthread_mutex_unlock(&o->mu);
	obj_put(o);
	return 0;
}

ssize_t timerfd_read(struct obj *o, void *buf, size_t n)
{
	uint64_t v;

	if (n < sizeof v)
		return fail(EINVAL);
	pthread_mutex_lock(&o->mu);
	for (;;) {
		if (o->closing) {
			pthread_mutex_unlock(&o->mu);
			return fail(EBADF);
		}
		tick(o, now_ns(o->u.tm.clock));
		if (o->u.tm.ticks)
			break;
		if (block_on(o) < 0) {
			pthread_mutex_unlock(&o->mu);
			return -1;
		}
	}
	v = o->u.tm.ticks;
	o->u.tm.ticks = 0;
	set_readable(o, false);
	pthread_cond_broadcast(&o->cv); /* the thread waits for the next one */
	pthread_mutex_unlock(&o->mu);
	memcpy(buf, &v, sizeof v);
	return sizeof v;
}
