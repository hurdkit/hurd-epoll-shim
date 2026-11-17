/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 7778777 */
/* Internal interface.  Every shim descriptor owns a private pipe that holds
 * exactly one byte while the object is readable; the descriptor handed to
 * the caller is a dup of the pipe's read end, so plain poll()/select() work
 * on it.  This file is compiled without the read/write/close/fcntl macros,
 * so those names are the real libc functions here. */
#ifndef SHIM_H
#define SHIM_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define EPOLL_SHIM_DISABLE_WRAPPER_MACROS
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define API __attribute__((visibility("default")))

#ifndef NSIG
#define NSIG 65
#endif

enum kind { K_EPOLL, K_EVENTFD, K_TIMERFD, K_SIGNALFD };

struct interest {
	int fd;
	uint32_t events;
	epoll_data_t data;
	unsigned seq; /* identifies this registration across unlocked polls */
	bool armed;   /* false once an EPOLLONESHOT entry has fired */
};

struct obj {
	enum kind kind;
	int refs, nfds; /* table lock: references / table slots */
	bool stale;     /* inherited across fork(): only close() works */
	int rp, wp;     /* private pipe, one byte in it iff readable */
	pthread_mutex_t mu;
	pthread_cond_t cv;
	bool readable, nonblock, closing, has_thread;
	pthread_t thread;
	union {
		struct {
			uint64_t count;
			bool sem;
		} ev;
		struct {
			clockid_t clock;
			bool armed;
			int64_t next, interval; /* ns on clock */
			uint64_t ticks;
		} tm;
		struct {
			sigset_t mask; /* guarded by the signal lock */
			struct signalfd_siginfo *q;
			int qlen;
			struct obj *next;
		} sg;
		struct {
			struct interest *it;
			int n, cap, rr, cr, cw; /* cr/cw: helper control pipe */
			unsigned seq, gen;
			bool ctl_full, polling, ready;
		} ep;
	} u;
};

/* core.c */
int fail(int err);
struct obj *obj_new(enum kind kind, bool nonblock);
int obj_publish(struct obj *o, bool cloexec);
void obj_abort(struct obj *o);
struct obj *obj_get(int fd);
struct obj *obj_get_kind(int fd, enum kind kind);
void obj_put(struct obj *o);
void set_readable(struct obj *o, bool on);
int block_on(struct obj *o);
int start_thread(pthread_t *t, void *(*fn)(void *), void *arg);
int64_t now_ns(clockid_t clock);
struct timespec cond_deadline(int64_t rel_ns);

/* per kind */
ssize_t eventfd_rw(struct obj *o, void *buf, size_t n, bool wr);
ssize_t timerfd_read(struct obj *o, void *buf, size_t n);
void timerfd_sync(struct obj *o);
ssize_t signalfd_read(struct obj *o, void *buf, size_t n);
void epoll_kick(struct obj *e);
void epoll_forget(struct obj *e, int fd);
void sig_unlink(struct obj *o);
void sig_lock(void);
void sig_unlock(void);
void sig_fork_child(void);

#endif
