/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Descriptor table, readiness pipes, read/write/close/fcntl interposition
 * and fork handling. */
#include "shim.h"

#include <fcntl.h>
#include <stdarg.h>

static pthread_mutex_t tab_mu = PTHREAD_MUTEX_INITIALIZER;
static struct obj **tab; /* indexed by descriptor number */
static int tabsz;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static clockid_t cond_clock = CLOCK_MONOTONIC;

API const char *epoll_shim_version(void)
{
	return "hurd-epoll-shim 0.1.0";
}

int fail(int err)
{
	errno = err;
	return -1;
}

static void close_private(struct obj *o)
{
	close(o->rp);
	close(o->wp);
	if (o->kind == K_EPOLL && o->u.ep.cr >= 0) {
		close(o->u.ep.cr);
		close(o->u.ep.cw);
	}
}

static void fork_prepare(void)
{
	sig_lock();
	pthread_mutex_lock(&tab_mu);
}

static void fork_parent(void)
{
	pthread_mutex_unlock(&tab_mu);
	sig_unlock();
}

/* The child has no helper threads, and locks held by other parent threads
 * stay held forever: inherited objects only support close(). */
static void fork_child(void)
{
	for (int i = 0; i < tabsz; i++)
		if (tab[i])
			tab[i]->stale = true;
	pthread_mutex_unlock(&tab_mu);
	sig_fork_child();
}

static void init(void)
{
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	if (pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) != 0)
		cond_clock = CLOCK_REALTIME;
	pthread_condattr_destroy(&ca);
	pthread_atfork(fork_prepare, fork_parent, fork_child);
}

int64_t now_ns(clockid_t clock)
{
	struct timespec ts;
	clock_gettime(clock, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Absolute condition-variable deadline rel_ns from now (capped at a day;
 * callers re-check their condition after every wakeup). */
struct timespec cond_deadline(int64_t rel_ns)
{
	const int64_t day = INT64_C(86400000000000);
	struct timespec ts;
	int64_t t = now_ns(cond_clock) +
	    (rel_ns < 0 ? 0 : rel_ns > day ? day : rel_ns);
	ts.tv_sec = (time_t)(t / 1000000000);
	ts.tv_nsec = (long)(t % 1000000000);
	return ts;
}

struct obj *obj_new(enum kind kind, bool nonblock)
{
	pthread_condattr_t ca;
	struct obj *o;
	int p[2];

	pthread_once(&once, init);
	if (!(o = calloc(1, sizeof *o))) {
		errno = ENOMEM;
		return NULL;
	}
	if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0) {
		free(o);
		return NULL;
	}
	o->kind = kind;
	o->rp = p[0];
	o->wp = p[1];
	o->nonblock = nonblock;
	if (kind == K_EPOLL)
		o->u.ep.cr = o->u.ep.cw = -1;
	pthread_mutex_init(&o->mu, NULL);
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, cond_clock);
	pthread_cond_init(&o->cv, &ca);
	pthread_condattr_destroy(&ca);
	return o;
}

static void obj_destroy(struct obj *o)
{
	close_private(o);
	if (o->kind == K_EPOLL)
		free(o->u.ep.it);
	else if (o->kind == K_SIGNALFD)
		free(o->u.sg.q);
	pthread_cond_destroy(&o->cv);
	pthread_mutex_destroy(&o->mu);
	free(o);
}

/* The last descriptor is gone: wake every waiter, stop the helper thread. */
static void obj_shutdown(struct obj *o)
{
	bool join;

	if (o->kind == K_SIGNALFD)
		sig_unlink(o);
	pthread_mutex_lock(&o->mu);
	if (o->kind == K_EPOLL)
		epoll_kick(o);
	set_readable(o, true); /* releases threads polling in block_on() */
	o->closing = true;
	pthread_cond_broadcast(&o->cv);
	join = o->has_thread;
	pthread_mutex_unlock(&o->mu);
	if (join)
		pthread_join(o->thread, NULL);
}

void obj_abort(struct obj *o)
{
	obj_shutdown(o);
	obj_destroy(o);
}

void obj_put(struct obj *o)
{
	bool last;
	pthread_mutex_lock(&tab_mu);
	last = --o->refs == 0 && !o->stale;
	pthread_mutex_unlock(&tab_mu);
	if (last)
		obj_destroy(o);
}

/* Drop one table slot's reference; o's slot is already cleared. */
static void release_slot(struct obj *o, bool last)
{
	if (o->stale)
		return; /* leaked on purpose: its locks may be held forever */
	if (last)
		obj_shutdown(o);
	obj_put(o);
}

static int tab_insert(int fd, struct obj *o)
{
	struct obj *old;
	bool last = false;

	pthread_mutex_lock(&tab_mu);
	if (fd >= tabsz) {
		int n = tabsz ? tabsz : 64;
		struct obj **t;
		while (n <= fd)
			n *= 2;
		if (!(t = realloc(tab, (size_t)n * sizeof *t))) {
			pthread_mutex_unlock(&tab_mu);
			return fail(ENOMEM);
		}
		memset(t + tabsz, 0, (size_t)(n - tabsz) * sizeof *t);
		tab = t;
		tabsz = n;
	}
	/* A previous object here was closed with the unwrapped close(). */
	if ((old = tab[fd]))
		last = --old->nfds == 0;
	tab[fd] = o;
	o->refs++;
	o->nfds++;
	pthread_mutex_unlock(&tab_mu);
	if (old)
		release_slot(old, last);
	return 0;
}

int obj_publish(struct obj *o, bool cloexec)
{
	int fd = fcntl(o->rp, cloexec ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
	int err = errno;

	if (fd >= 0 && tab_insert(fd, o) == 0)
		return fd;
	if (fd >= 0) {
		close(fd);
		err = ENOMEM;
	}
	obj_abort(o);
	return fail(err);
}

struct obj *obj_get(int fd)
{
	struct obj *o = NULL;
	pthread_mutex_lock(&tab_mu);
	if (fd >= 0 && fd < tabsz && (o = tab[fd]))
		o->refs++;
	pthread_mutex_unlock(&tab_mu);
	return o;
}

/* Reference to a usable object of the given kind, or NULL with errno set to
 * EBADF (not a descriptor) or EINVAL (some other kind of descriptor). */
struct obj *obj_get_kind(int fd, enum kind kind)
{
	struct obj *o = obj_get(fd);
	int err;

	if (o && o->kind == kind && !o->stale)
		return o;
	if (o) {
		err = o->stale ? EBADF : EINVAL;
		obj_put(o);
	} else {
		err = fcntl(fd, F_GETFD) < 0 ? EBADF : EINVAL;
	}
	errno = err;
	return NULL;
}

/* o->mu held.  Keep exactly one byte in the pipe while readable.  After
 * shutdown the pipe stays readable so that every waiter wakes up. */
void set_readable(struct obj *o, bool on)
{
	char c = 0;
	if (o->closing || o->readable == on)
		return;
	if (on ? write(o->wp, &c, 1) == 1 :
		 (read(o->rp, &c, 1) == 1 || errno == EAGAIN))
		o->readable = on;
}

/* o->mu held, object not ready: wait until it becomes readable.  Returns 0
 * to re-check, or -1 with EAGAIN, EINTR (signal handler ran) or EBADF. */
int block_on(struct obj *o)
{
	struct pollfd p = { o->rp, POLLIN, 0 };
	int r, err;

	if (o->closing)
		return fail(EBADF);
	if (o->nonblock)
		return fail(EAGAIN);
	pthread_mutex_unlock(&o->mu);
	r = poll(&p, 1, -1);
	err = errno;
	pthread_mutex_lock(&o->mu);
	if (r < 0)
		return fail(err);
	return o->closing ? fail(EBADF) : 0;
}

/* Helper threads run with every signal blocked; the caller's mask is
 * restored before returning. */
int start_thread(pthread_t *t, void *(*fn)(void *), void *arg)
{
	sigset_t all, old;
	int err;

	sigfillset(&all);
	pthread_sigmask(SIG_SETMASK, &all, &old);
	err = pthread_create(t, NULL, fn, arg);
	pthread_sigmask(SIG_SETMASK, &old, NULL);
	return err ? fail(err) : 0;
}

static ssize_t rw(int fd, void *buf, size_t n, bool wr)
{
	struct obj *o = obj_get(fd);
	ssize_t r;
	int err;

	if (!o)
		return wr ? write(fd, buf, n) : read(fd, buf, n);
	if (o->stale)
		r = fail(EBADF);
	else if (o->kind == K_EVENTFD)
		r = eventfd_rw(o, buf, n, wr);
	else if (o->kind == K_TIMERFD && !wr)
		r = timerfd_read(o, buf, n);
	else if (o->kind == K_SIGNALFD && !wr)
		r = signalfd_read(o, buf, n);
	else
		r = fail(EINVAL); /* epoll fds; writes to timerfd/signalfd */
	err = errno;
	obj_put(o);
	errno = err;
	return r;
}

API ssize_t epoll_shim_read(int fd, void *buf, size_t n)
{
	return rw(fd, buf, n, false);
}

API ssize_t epoll_shim_write(int fd, const void *buf, size_t n)
{
	return rw(fd, (void *)buf, n, true);
}

API int epoll_shim_close(int fd)
{
	struct obj *o = NULL;
	bool last = false;
	int r, err;

	pthread_mutex_lock(&tab_mu);
	if (fd >= 0 && fd < tabsz && (o = tab[fd])) {
		tab[fd] = NULL;
		last = --o->nfds == 0;
		if (o->stale && last)
			close_private(o);
	}
	/* Linux drops a closed file from every epoll set.  Do it before the
	 * number can be reused, so a new file never inherits a registration. */
	for (int i = 0; i < tabsz; i++) {
		struct obj *e = tab[i];
		if (e && e->kind == K_EPOLL && !e->stale) {
			pthread_mutex_lock(&e->mu);
			epoll_forget(e, fd);
			pthread_mutex_unlock(&e->mu);
		}
	}
	pthread_mutex_unlock(&tab_mu);
	r = close(fd);
	if (o) {
		err = errno;
		release_slot(o, last);
		errno = err;
	}
	return r;
}

API int epoll_shim_fcntl(int fd, int cmd, ...)
{
	bool noarg = cmd == F_GETFD || cmd == F_GETFL;
	bool intarg = cmd == F_SETFD || cmd == F_SETFL || cmd == F_DUPFD ||
	    cmd == F_DUPFD_CLOEXEC;
	struct obj *o;
	void *p = NULL;
	int a = 0, r, err;
	va_list ap;

	/* Like glibc's own fcntl, other commands take their argument as a
	 * pointer-sized value; that also carries int arguments on x86. */
	va_start(ap, cmd);
	if (intarg)
		a = va_arg(ap, int);
	else if (!noarg)
		p = va_arg(ap, void *);
	va_end(ap);

	o = obj_get(fd);
	if (o && o->stale) {
		r = fail(EBADF);
	} else if (o && cmd == F_GETFL) {
		/* report O_RDWR and the emulated O_NONBLOCK like Linux */
		r = fcntl(fd, F_GETFL);
		pthread_mutex_lock(&o->mu);
		if (r >= 0)
			r = (r & ~(O_ACCMODE | O_NONBLOCK)) | O_RDWR |
			    (o->nonblock ? O_NONBLOCK : 0);
		pthread_mutex_unlock(&o->mu);
	} else if (o && cmd == F_SETFL) {
		/* the pipe itself always stays nonblocking */
		r = fcntl(fd, F_SETFL, a | O_NONBLOCK);
		pthread_mutex_lock(&o->mu);
		if (r == 0)
			o->nonblock = (a & O_NONBLOCK) != 0;
		pthread_mutex_unlock(&o->mu);
	} else if (o && (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)) {
		/* both descriptors name the same object */
		r = fcntl(fd, cmd, a);
		if (r >= 0 && tab_insert(r, o) < 0) {
			close(r);
			r = fail(ENOMEM);
		}
	} else { /* plain fds, and F_GETFD/F_SETFD etc. on the real pipe */
		r = noarg ? fcntl(fd, cmd) :
		    intarg ? fcntl(fd, cmd, a) : fcntl(fd, cmd, p);
	}
	if (!o)
		return r;
	err = errno;
	obj_put(o);
	errno = err;
	return r;
}
