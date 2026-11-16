/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/*
 * signalfd.  Linux requires the caller to block the signals, which then stay
 * pending until a signalfd reads them.  Here one process-wide helper thread
 * is the only thread with those signals unblocked.  A minimal SA_SIGINFO
 * handler (it only calls write(), which is async-signal-safe) forwards each
 * delivery through a pipe, and the helper queues it into every signalfd
 * whose mask contains it.  Replaced dispositions are restored once no
 * signalfd wants the signal any more.
 *
 * sigwaitinfo()/sigtimedwait() are not used: GNU/Hurd's implementation
 * returns an already-pending signal without dequeuing it (a waiter would
 * spin on it forever), glibc 2.41's sigtimedwait timeout path leaves a
 * dangling preemptor behind, and Hurd has no real-time signal that could be
 * reserved to wake the waiter when a mask changes.
 */
#include "shim.h"

#include <fcntl.h>

_Static_assert(sizeof(struct signalfd_siginfo) == 128, "Linux layout");

struct rec { /* handler -> helper; all int64 so there is no padding */
	int64_t signo, code, err, pid, uid, status, ival;
	uint64_t ptr;
};

static pthread_mutex_t sig_mu = PTHREAD_MUTEX_INITIALIZER;
static struct obj *sig_list; /* live signalfds */
static int sig_r = -1, sig_w = -1;
static bool sig_running;
static bool installed[NSIG];
static struct sigaction saved[NSIG]; /* the dispositions we replaced */

void sig_lock(void)
{
	pthread_mutex_lock(&sig_mu);
}

void sig_unlock(void)
{
	pthread_mutex_unlock(&sig_mu);
}

static void on_signal(int s, siginfo_t *si, void *uc)
{
	int saved_errno = errno;
	struct rec r;
	ssize_t w;

	(void)uc;
	r.signo = s;
	r.code = si->si_code;
	r.err = si->si_errno;
	r.pid = si->si_pid;
	r.uid = si->si_uid;
	r.status = si->si_status;
	r.ival = si->si_value.sival_int;
	r.ptr = (uint64_t)(uintptr_t)si->si_value.sival_ptr;
	w = write(sig_w, &r, sizeof r); /* atomic: sizeof r < PIPE_BUF */
	(void)w;
	errno = saved_errno;
}

static void wake(void)
{
	struct rec r = { 0 };
	ssize_t w = write(sig_w, &r, sizeof r);
	(void)w;
}

/* sig_mu held.  Queue r into every signalfd that wants it. */
static bool deliver(const struct rec *r)
{
	bool any = false;

	for (struct obj *o = sig_list; o; o = o->u.sg.next) {
		struct signalfd_siginfo *x;
		int i;

		if (sigismember(&o->u.sg.mask, (int)r->signo) != 1)
			continue;
		any = true;
		pthread_mutex_lock(&o->mu);
		for (i = 0; i < o->u.sg.qlen; i++)
			if (o->u.sg.q[i].ssi_signo == r->signo)
				break; /* like standard signals, do not queue twice */
		if (i == o->u.sg.qlen && i < NSIG) {
			x = &o->u.sg.q[o->u.sg.qlen++];
			memset(x, 0, sizeof *x);
			x->ssi_signo = (uint32_t)r->signo;
			x->ssi_errno = (int32_t)r->err;
			x->ssi_code = (int32_t)r->code;
			x->ssi_pid = (uint32_t)r->pid;
			x->ssi_uid = (uint32_t)r->uid;
			if (r->signo == SIGCHLD)
				x->ssi_status = (int32_t)r->status;
			if (r->code == SI_QUEUE || r->code == SI_TIMER ||
			    r->code == SI_MESGQ) {
				x->ssi_int = (int32_t)r->ival;
				x->ssi_ptr = r->ptr;
			}
			set_readable(o, true);
		}
		pthread_mutex_unlock(&o->mu);
	}
	return any;
}

static void *sig_main(void *arg)
{
	sigset_t want, block, again;
	struct rec r;

	(void)arg;
	sigemptyset(&again);
	for (;;) {
		pthread_mutex_lock(&sig_mu);
		sigemptyset(&want);
		for (struct obj *o = sig_list; o; o = o->u.sg.next)
			for (int s = 1; s < NSIG; s++)
				if (sigismember(&o->u.sg.mask, s) == 1)
					sigaddset(&want, s);
		sigfillset(&block);
		for (int s = 1; s < NSIG; s++)
			if (sigismember(&want, s) == 1)
				sigdelset(&block, s);
		/* Exactly the wanted signals are unblocked here; pending ones
		 * are delivered right away. */
		pthread_sigmask(SIG_SETMASK, &block, NULL);
		for (int s = 1; s < NSIG; s++) {
			if (installed[s] && sigismember(&want, s) != 1) {
				sigaction(s, &saved[s], NULL);
				installed[s] = false;
			}
			if (sigismember(&again, s) == 1)
				kill(getpid(), s); /* unwanted now: give it back */
		}
		sigemptyset(&again);
		pthread_mutex_unlock(&sig_mu);

		if (read(sig_r, &r, sizeof r) != (ssize_t)sizeof r ||
		    r.signo <= 0 || r.signo >= NSIG)
			continue; /* interrupted, or a wake-up after a mask change */
		pthread_mutex_lock(&sig_mu);
		if (!deliver(&r))
			sigaddset(&again, (int)r.signo);
		pthread_mutex_unlock(&sig_mu);
	}
	return NULL;
}

/* sig_mu held */
static int setup(void)
{
	pthread_t t;
	int p[2];

	if (sig_running)
		return 0;
	if (sig_r < 0) {
		if (pipe2(p, O_CLOEXEC) < 0)
			return -1;
		fcntl(p[1], F_SETFL, O_NONBLOCK); /* the handler never blocks */
		sig_r = p[0];
		sig_w = p[1];
	}
	if (start_thread(&t, sig_main, NULL) < 0)
		return -1;
	pthread_detach(t);
	sig_running = true;
	return 0;
}

/* sig_mu held */
static void install(sigset_t *m)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_signal;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigfillset(&sa.sa_mask);
	for (int s = 1; s < NSIG; s++) {
		if (sigismember(m, s) != 1 || installed[s])
			continue;
		if (sigaction(s, &sa, &saved[s]) == 0)
			installed[s] = true;
		else
			sigdelset(m, s);
	}
}

/* Signals a signalfd can carry: not SIGKILL/SIGSTOP (Linux ignores them),
 * not the synchronous fault signals (a handler would loop on the fault). */
static bool usable(int s)
{
	return s != SIGKILL && s != SIGSTOP && s != SIGSEGV && s != SIGBUS &&
	    s != SIGILL && s != SIGFPE && s != SIGTRAP;
}

API int signalfd(int fd, const sigset_t *mask, int flags)
{
	struct obj *o;
	sigset_t m;
	int r;

	if (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC))
		return fail(EINVAL);
	if (!mask)
		return fail(EFAULT);
	sigemptyset(&m);
	for (int s = 1; s < NSIG; s++)
		if (sigismember(mask, s) == 1 && usable(s))
			sigaddset(&m, s);

	if (fd != -1) { /* change the mask of an existing signalfd */
		if (!(o = obj_get_kind(fd, K_SIGNALFD)))
			return -1;
		pthread_mutex_lock(&sig_mu);
		install(&m);
		o->u.sg.mask = m;
		wake();
		pthread_mutex_unlock(&sig_mu);
		obj_put(o);
		return fd;
	}
	if (!(o = obj_new(K_SIGNALFD, flags & SFD_NONBLOCK)))
		return -1;
	if (!(o->u.sg.q = calloc(NSIG, sizeof *o->u.sg.q))) {
		obj_abort(o);
		return fail(ENOMEM);
	}
	pthread_mutex_lock(&sig_mu);
	if ((r = setup()) == 0) {
		install(&m);
		o->u.sg.mask = m;
		o->u.sg.next = sig_list;
		sig_list = o;
		wake();
	}
	pthread_mutex_unlock(&sig_mu);
	if (r < 0) {
		r = errno;
		obj_abort(o);
		return fail(r);
	}
	return obj_publish(o, flags & SFD_CLOEXEC);
}

void sig_unlink(struct obj *o)
{
	pthread_mutex_lock(&sig_mu);
	for (struct obj **pp = &sig_list; *pp; pp = &(*pp)->u.sg.next) {
		if (*pp == o) {
			*pp = o->u.sg.next;
			wake();
			break;
		}
	}
	pthread_mutex_unlock(&sig_mu);
}

/* fork child, sig_mu held since fork_prepare */
void sig_fork_child(void)
{
	for (int s = 1; s < NSIG; s++) {
		if (installed[s]) {
			sigaction(s, &saved[s], NULL);
			installed[s] = false;
		}
	}
	if (sig_r >= 0) {
		close(sig_r);
		close(sig_w);
		sig_r = sig_w = -1;
	}
	sig_list = NULL;
	sig_running = false;
	pthread_mutex_unlock(&sig_mu);
}

ssize_t signalfd_read(struct obj *o, void *buf, size_t n)
{
	const size_t rec = sizeof(struct signalfd_siginfo);
	size_t k = n / rec;

	if (k == 0)
		return fail(EINVAL);
	pthread_mutex_lock(&o->mu);
	while (o->u.sg.qlen == 0) {
		if (block_on(o) < 0) {
			pthread_mutex_unlock(&o->mu);
			return -1;
		}
	}
	if (k > (size_t)o->u.sg.qlen)
		k = (size_t)o->u.sg.qlen;
	memcpy(buf, o->u.sg.q, k * rec);
	o->u.sg.qlen -= (int)k;
	memmove(o->u.sg.q, o->u.sg.q + k, (size_t)o->u.sg.qlen * rec);
	set_readable(o, o->u.sg.qlen > 0);
	pthread_mutex_unlock(&o->mu);
	return (ssize_t)(k * rec);
}
