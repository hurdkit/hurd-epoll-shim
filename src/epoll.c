/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/*
 * epoll.  epoll_wait() polls the interest list with poll(..., 0) and, when
 * nothing is ready, sleeps in poll() on the epoll object's own pipe.  A
 * helper thread per epoll object keeps that pipe's readability equal to "some
 * entry is ready": it polls the interest list plus a control pipe; when an
 * entry is ready it marks the epoll object readable and parks until
 * epoll_wait/epoll_ctl/close changes something (a "kick"), which also clears
 * readability.  Nothing ready means the helper is blocked in poll().
 */
#include "shim.h"

#include <limits.h>

static short to_poll(uint32_t ev)
{
	short p = 0;
	if (ev & (EPOLLIN | EPOLLRDNORM))
		p |= POLLIN;
	if (ev & (EPOLLOUT | EPOLLWRNORM))
		p |= POLLOUT;
	if (ev & EPOLLPRI)
		p |= POLLPRI;
#ifdef POLLRDHUP
	if (ev & EPOLLRDHUP)
		p |= POLLRDHUP;
#endif
	return p;
}

/* Requested events plus EPOLLERR/EPOLLHUP; EPOLLRDHUP only if requested. */
static uint32_t from_poll(short re, uint32_t want)
{
	uint32_t ev = 0;
	if (re & (POLLIN | POLLRDNORM))
		ev |= EPOLLIN | EPOLLRDNORM;
	if (re & (POLLOUT | POLLWRNORM))
		ev |= EPOLLOUT | EPOLLWRNORM;
	if (re & POLLPRI)
		ev |= EPOLLPRI;
	if (re & POLLERR)
		ev |= EPOLLERR;
	if (re & POLLHUP)
		ev |= EPOLLHUP | EPOLLRDHUP;
#ifdef POLLRDHUP
	if (re & POLLRDHUP)
		ev |= EPOLLRDHUP;
#endif
	return ev & (want | EPOLLERR | EPOLLHUP);
}

static int find(struct obj *e, int fd)
{
	for (int i = 0; i < e->u.ep.n; i++)
		if (e->u.ep.it[i].fd == fd)
			return i;
	return -1;
}

static void forget_at(struct obj *e, int i)
{
	e->u.ep.it[i] = e->u.ep.it[--e->u.ep.n];
}

/* e->mu held: something changed.  Clear readiness and make the helper
 * re-evaluate (via the control pipe if it is inside poll()). */
void epoll_kick(struct obj *e)
{
	char c = 0;

	e->u.ep.ready = false;
	set_readable(e, false);
	e->u.ep.gen++;
	if (e->u.ep.polling && !e->u.ep.ctl_full &&
	    write(e->u.ep.cw, &c, 1) == 1)
		e->u.ep.ctl_full = true;
	pthread_cond_broadcast(&e->cv);
}

/* e->mu held: fd is being closed. */
void epoll_forget(struct obj *e, int fd)
{
	int i = find(e, fd);
	if (i >= 0) {
		forget_at(e, i);
		epoll_kick(e);
	}
}

/* e->mu held: pollfds for the armed entries; seq[] remembers which
 * registration each slot belongs to.  Returns the count. */
static int snapshot(struct obj *e, struct pollfd *pf, unsigned *seq)
{
	int k = 0;
	for (int i = 0; i < e->u.ep.n; i++) {
		struct interest *it = &e->u.ep.it[i];
		if (!it->armed)
			continue;
		pf[k].fd = it->fd;
		pf[k].events = to_poll(it->events);
		pf[k].revents = 0;
		seq[k++] = it->seq;
	}
	return k;
}

/* e->mu held: map poll results back onto entries that still exist.  With
 * out == NULL only report whether anything is ready.  Entries whose fd was
 * closed behind the shim's back (POLLNVAL) are dropped. */
static int scan(struct obj *e, struct pollfd *pf, unsigned *seq, int k,
    struct epoll_event *out, int max)
{
	int cnt = 0, start = out && k ? e->u.ep.rr % k : 0;

	for (int i = 0; i < k && (!out || cnt < max); i++) {
		int j = (start + i) % k, x;
		struct interest *it;
		uint32_t ev;

		if (!pf[j].revents || (x = find(e, pf[j].fd)) < 0)
			continue;
		it = &e->u.ep.it[x];
		if (it->seq != seq[j] || !it->armed)
			continue;
		if (pf[j].revents & POLLNVAL) {
			forget_at(e, x);
			continue;
		}
		if (!(ev = from_poll(pf[j].revents, it->events)))
			continue;
		if (!out)
			return 1;
		out[cnt].events = ev;
		out[cnt].data = it->data;
		cnt++;
		if (it->events & EPOLLONESHOT)
			it->armed = false;
		e->u.ep.rr = j + 1;
	}
	return cnt;
}

/* On GNU/Hurd a poll() can stop returning altogether when another thread
 * closes one of its descriptors, even after another of them becomes ready;
 * and it returns at once, with nothing ready, for events a descriptor
 * cannot produce (EPOLLPRI on a pipe).  There the helper polls with a
 * timeout, so that it looks at its set again at least every
 * HELPER_POLL_MS, and waits instead of polling again after an early empty
 * return, so that it does not spin. */
#ifdef __GNU__
#define HELPER_POLL_MS 1000
#else
#define HELPER_POLL_MS (-1)
#endif

static void *epoll_main(void *arg)
{
	struct obj *e = arg;
	struct pollfd *pf = NULL;
	unsigned *seq;
	int cap = 0;

	pthread_mutex_lock(&e->mu);
	while (!e->closing) {
		unsigned gen = e->u.ep.gen;
		bool ready = false, park = false, early = false;
		int64_t start;
		char c;
		int k, r, err;

		if (e->u.ep.ctl_full) /* drain the kick byte */
			e->u.ep.ctl_full = read(e->u.ep.cr, &c, 1) != 1;
		if (e->u.ep.n + 1 > cap) {
			int ncap = e->u.ep.n + 16;
			void *n = realloc(pf, (size_t)ncap * (sizeof *pf + sizeof *seq));
			if (!n) { /* retry after the next change */
				while (!e->closing && e->u.ep.gen == gen)
					pthread_cond_wait(&e->cv, &e->mu);
				continue;
			}
			pf = n;
			cap = ncap;
		}
		seq = (unsigned *)(pf + cap);
		k = snapshot(e, pf, seq);
		pf[k].fd = e->u.ep.cr;
		pf[k].events = POLLIN;
		pf[k].revents = 0;
		e->u.ep.polling = true;
		pthread_mutex_unlock(&e->mu);
		start = now_ns(CLOCK_MONOTONIC);
		r = poll(pf, (nfds_t)k + 1, HELPER_POLL_MS);
		err = errno;
		pthread_mutex_lock(&e->mu);
		e->u.ep.polling = false;
		if (r > 0)
			ready = scan(e, pf, seq, k, NULL, 0) > 0;
		else if (r < 0 && err != EINTR)
			park = true; /* e.g. too many fds: wait for a change */
		else if (r == 0 && HELPER_POLL_MS > 0 &&
		    now_ns(CLOCK_MONOTONIC) - start <
		    (int64_t)HELPER_POLL_MS * 500000)
			early = true;
		if (ready && !e->u.ep.ready) {
			e->u.ep.ready = true;
			set_readable(e, true);
		}
		if (ready || park)
			while (!e->closing && e->u.ep.gen == gen)
				pthread_cond_wait(&e->cv, &e->mu);
		if (early) {
			struct timespec until =
			    cond_deadline((int64_t)HELPER_POLL_MS * 1000000);
			while (!e->closing && e->u.ep.gen == gen &&
			    pthread_cond_timedwait(&e->cv, &e->mu, &until) == 0)
				;
		}
	}
	pthread_mutex_unlock(&e->mu);
	free(pf);
	return NULL;
}

API int epoll_create1(int flags)
{
	struct obj *o;
	int p[2], err;

	if (flags & ~EPOLL_CLOEXEC)
		return fail(EINVAL);
	if (!(o = obj_new(K_EPOLL, false)))
		return -1;
	if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0)
		goto fail;
	o->u.ep.cr = p[0];
	o->u.ep.cw = p[1];
	if (start_thread(&o->thread, epoll_main, o) < 0)
		goto fail;
	o->has_thread = true;
	return obj_publish(o, flags & EPOLL_CLOEXEC);
fail:
	err = errno;
	obj_abort(o);
	return fail(err);
}

API int epoll_create(int size)
{
	return size <= 0 ? fail(EINVAL) : epoll_create1(0);
}

static int ctl(struct obj *e, int op, int fd, struct epoll_event *ev)
{
	int x, r = 0;

	pthread_mutex_lock(&e->mu);
	x = find(e, fd);
	if (e->closing) {
		r = fail(EBADF);
	} else if (op == EPOLL_CTL_ADD) {
		if (x >= 0) {
			r = fail(EEXIST);
		} else if (e->u.ep.n == e->u.ep.cap) {
			int ncap = e->u.ep.cap ? 2 * e->u.ep.cap : 8;
			struct interest *n = realloc(e->u.ep.it, (size_t)ncap * sizeof *n);
			if (!n)
				r = fail(ENOMEM);
			else {
				e->u.ep.it = n;
				e->u.ep.cap = ncap;
			}
		}
		if (r == 0) {
			struct interest *it = &e->u.ep.it[e->u.ep.n++];
			it->fd = fd;
			it->events = ev->events;
			it->data = ev->data;
			it->seq = ++e->u.ep.seq;
			it->armed = true;
		}
	} else if (x < 0) {
		r = fail(ENOENT);
	} else if (op == EPOLL_CTL_MOD) {
		e->u.ep.it[x].events = ev->events;
		e->u.ep.it[x].data = ev->data;
		e->u.ep.it[x].armed = true;
	} else {
		forget_at(e, x);
	}
	if (r == 0)
		epoll_kick(e);
	pthread_mutex_unlock(&e->mu);
	return r;
}

API int epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev)
{
	struct obj *e, *t;
	int r, err;

	if (!(e = obj_get_kind(epfd, K_EPOLL)))
		return -1;
	t = obj_get(fd);
	if (!t && fcntl(fd, F_GETFD) < 0)
		r = fail(EBADF);
	else if (t == e || fd == epfd)
		r = fail(EINVAL);
	else if (op != EPOLL_CTL_ADD && op != EPOLL_CTL_MOD && op != EPOLL_CTL_DEL)
		r = fail(EINVAL);
	else if (op != EPOLL_CTL_DEL && !ev)
		r = fail(EFAULT);
	else
		r = ctl(e, op, fd, ev);
	err = errno;
	if (t)
		obj_put(t);
	obj_put(e);
	errno = err;
	return r;
}

/* No lock held.  A reference to the usable shim object behind fd, or NULL. */
static struct obj *shim_obj(int fd)
{
	struct obj *o = obj_get(fd);

	if (o && o->stale) {
		obj_put(o);
		o = NULL;
	}
	return o;
}

/* No lock held: poll the snapshot without blocking, as the kernel would
 * report it at this instant.  The pipes of timerfds and signalfds are
 * written by helper threads, which lag behind the state they publish:
 *
 * - a timer that is due may not have been marked readable yet, so every
 *   timerfd is brought up to date first;
 * - one signal is queued into the signalfds that want it one after the
 *   other, under the signal lock.  When a signalfd is ready, pass through
 *   that lock, so that a delivery in progress reaches all of them, and
 *   look again. */
static int poll_now(struct pollfd *pf, int k)
{
	struct obj *o;
	bool sig = false;
	int r;

	for (int i = 0; i < k; i++) {
		if (!(o = shim_obj(pf[i].fd)))
			continue;
		if (o->kind == K_TIMERFD)
			timerfd_sync(o);
		obj_put(o);
	}
	if ((r = poll(pf, (nfds_t)k, 0)) <= 0)
		return r;
	for (int i = 0; i < k && !sig; i++) {
		if (!pf[i].revents || !(o = shim_obj(pf[i].fd)))
			continue;
		sig = o->kind == K_SIGNALFD;
		obj_put(o);
	}
	if (!sig)
		return r;
	sig_lock();
	sig_unlock();
	return poll(pf, (nfds_t)k, 0);
}

/* e->mu held: report ready entries without blocking. */
static int collect(struct obj *e, struct epoll_event *out, int max)
{
	int n = e->u.ep.n, k, r = 0;
	struct pollfd *pf;
	unsigned *seq;

	if (n == 0)
		return 0;
	if (!(pf = malloc((size_t)n * (sizeof *pf + sizeof *seq))))
		return fail(ENOMEM);
	seq = (unsigned *)(pf + n);
	if ((k = snapshot(e, pf, seq)) > 0) {
		pthread_mutex_unlock(&e->mu);
		r = poll_now(pf, k);
		pthread_mutex_lock(&e->mu);
		r = r > 0 ? scan(e, pf, seq, k, out, max) : 0;
	}
	free(pf);
	return r;
}

static int ep_wait(int epfd, struct epoll_event *out, int max, int timeout)
{
	struct obj *e;
	int64_t deadline = 0;
	int r, err;

	if (max <= 0)
		return fail(EINVAL);
	if (!(e = obj_get_kind(epfd, K_EPOLL)))
		return -1;
	if (timeout > 0)
		deadline = now_ns(CLOCK_MONOTONIC) + (int64_t)timeout * 1000000;
	pthread_mutex_lock(&e->mu);
	for (;;) {
		struct pollfd p = { e->rp, POLLIN, 0 };
		int64_t left = deadline - now_ns(CLOCK_MONOTONIC);
		int ms = timeout < 0 ? -1 :
		    left <= 0 ? 0 : (int)((left + 999999) / 1000000);

		if (e->closing) {
			r = fail(EBADF);
			break;
		}
		if ((r = collect(e, out, max)) != 0)
			break;
		if (e->u.ep.ready)
			epoll_kick(e); /* stale: nothing to report any more */
		if (ms == 0)
			break;
		pthread_mutex_unlock(&e->mu);
		r = poll(&p, 1, ms); /* the helper makes rp readable */
		err = errno;
		pthread_mutex_lock(&e->mu);
		if (r < 0) {
			r = fail(err);
			break;
		}
	}
	if (r > 0)
		epoll_kick(e); /* the caller will consume; re-evaluate */
	err = errno;
	pthread_mutex_unlock(&e->mu);
	obj_put(e);
	errno = err;
	return r;
}

API int epoll_wait(int epfd, struct epoll_event *events, int maxevents,
    int timeout)
{
	return ep_wait(epfd, events, maxevents, timeout);
}

/* The mask is applied around the wait with pthread_sigmask(), so it is not
 * atomic: a signal unblocked by sigmask that arrives before the wait starts
 * runs its handler but does not make this call return EINTR. */
API int epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
    int timeout, const sigset_t *sigmask)
{
	sigset_t old;
	int r, err;

	if (sigmask && (err = pthread_sigmask(SIG_SETMASK, sigmask, &old)))
		return fail(err);
	r = ep_wait(epfd, events, maxevents, timeout);
	if (sigmask) {
		err = errno;
		pthread_sigmask(SIG_SETMASK, &old, NULL);
		errno = err;
	}
	return r;
}
