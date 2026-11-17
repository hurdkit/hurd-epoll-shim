# hurd-epoll-shim

Linux `epoll`, `eventfd`, `timerfd` and `signalfd` for GNU/Hurd, written for
the libwayland port. It uses only POSIX and pthreads, and it is MIT licensed.

## How it works

Every shim descriptor is a duplicate of the read end of a private pipe. The
pipe holds one byte exactly while the object is readable. A plain `poll()` or
`select()` therefore works on shim descriptors, with no wrapping. That
includes an epoll descriptor inside another event loop, or inside another
epoll set.

- **eventfd:** a counter guarded by a mutex.
- **timerfd:** one helper thread per armed timer. It sleeps on a
  `CLOCK_MONOTONIC` condition variable.
- **epoll:** one helper thread per instance. It `poll()`s the interest list
  and a control pipe, then marks the epoll descriptor readable while an entry
  is ready. With nothing ready, every thread is blocked. `epoll_wait` does
  not rely on the other helpers being on time: it accounts for timers that
  are due, and when a signalfd is ready it lets the delivery of that signal
  reach every signalfd before it reports.
- **signalfd:** one process-wide helper thread. It is the only thread that
  unblocks the signals named in signalfd masks. The shim installs a small
  `SA_SIGINFO` handler for those signals. The handler only calls `write()`
  into a pipe, and the helper reads that pipe and queues each signal into the
  matching signalfds. When no signalfd wants a signal any more, the previous
  disposition is restored. When the last signalfd is closed, the helper and
  its pipe go away, so the process holds no descriptor of the shim's.

  `sigwaitinfo()` and `sigtimedwait()` are not used, for three reasons. Hurd
  glibc returns an already-pending signal from them without dequeuing it.
  The sigtimedwait timeout path in glibc 2.41 is broken. Hurd has no
  real-time signal that could be reserved for waking the helper.

## Interposition

The headers are `<sys/epoll.h>`, `<sys/eventfd.h>`, `<sys/timerfd.h>` and
`<sys/signalfd.h>`, all under `include/hurd-epoll-shim`. They declare the
Linux API. Each one also includes `<epoll-shim/wrap.h>`, which defines these
macros:

```c
#define read(...)  epoll_shim_read(__VA_ARGS__)
#define write(...) epoll_shim_write(__VA_ARGS__)
#define close(...) epoll_shim_close(__VA_ARGS__)
#define fcntl(...) epoll_shim_fcntl(__VA_ARGS__)
```

The wrappers handle shim descriptors themselves and pass every other
descriptor on to libc. `close()` on any descriptor also removes that number
from every epoll set, as Linux does.

`poll` and `select` are not wrapped. `dup`, `dup2` and `dup3` are not wrapped
either, so use `fcntl(fd, F_DUPFD[_CLOEXEC], n)` to duplicate a shim
descriptor.

The macros have consequences for code that uses these headers:

- Every source file that reads, writes, closes or `fcntl`s a shim descriptor
  must include one of these headers.
- Code that has struct members or methods named `read`, `write`, `close` or
  `fcntl` must `#undef` the macros after the include.
- Alternatively, define `EPOLL_SHIM_DISABLE_WRAPPER_MACROS` and call the
  `epoll_shim_*` functions directly.

The headers also define `HURD_EPOLL_SHIM`, and `epoll_shim_version()` returns
a version string.

## Supported

| Call | Supported |
| --- | --- |
| `epoll_create`, `epoll_create1` | `EPOLL_CLOEXEC` |
| `epoll_ctl` | `ADD`, `MOD`, `DEL`, with Linux error codes |
| `epoll_wait`, `epoll_pwait` | Events: `EPOLLIN`, `EPOLLOUT`, `EPOLLPRI`, `EPOLLERR`, `EPOLLHUP`, `EPOLLRDHUP` (only when requested) and `EPOLLONESHOT` |
| `eventfd`, `eventfd_read`, `eventfd_write` | `EFD_SEMAPHORE`, `EFD_NONBLOCK`, `EFD_CLOEXEC` |
| `timerfd_create`, `timerfd_settime`, `timerfd_gettime` | Clocks: `CLOCK_MONOTONIC`, `CLOCK_REALTIME`, `CLOCK_BOOTTIME`. Flags: `TFD_NONBLOCK`, `TFD_CLOEXEC`, `TFD_TIMER_ABSTIME`. Periodic timers count overruns. |
| `signalfd` | `SFD_NONBLOCK`, `SFD_CLOEXEC`, and changing the mask of an existing descriptor |

`fcntl` on a shim descriptor behaves as follows:

- `F_GETFD` and `F_SETFD` act on the real descriptor.
- `F_GETFL` reports `O_RDWR` plus the emulated `O_NONBLOCK`.
- `F_SETFL` toggles the emulated `O_NONBLOCK`.
- `F_DUPFD` and `F_DUPFD_CLOEXEC` return a second descriptor for the same
  object.

## Limitations

**epoll**

- `EPOLLET` is treated as level-triggered.
- `EPOLLEXCLUSIVE` and `EPOLLWAKEUP` are ignored.
- Epoll cycles are not rejected with `ELOOP`.
- Registrations are keyed by descriptor number. Closing a number through the
  wrapper drops it even if a `dup` of the file is still open, whereas Linux
  keeps it until the last duplicate is closed.
- A descriptor closed with the unwrapped `close()` is dropped when `poll()`
  reports `POLLNVAL`. If its number is reused before that, the new file
  inherits the old registration.
- On GNU/Hurd, a `poll()` can stop returning when another thread closes one
  of its descriptors, even after another of them becomes ready, and it
  returns at once for events a descriptor cannot produce (`EPOLLPRI` on a
  pipe). The helper thread therefore polls with a one-second timeout there:
  after an unwrapped `close()` of a member, the epoll descriptor can be up to
  a second late, and an idle epoll instance wakes once a second.
- An epoll descriptor becomes readable a thread hand-off after a member
  becomes ready.
- After `epoll_wait()` returns events, the epoll descriptor is unreadable for
  a moment until the helper checks again.
- If you drain a member without calling `epoll_wait`/`epoll_ctl`, the epoll
  descriptor stays readable until the next `epoll_wait`. That causes one
  spurious wakeup.
- `epoll_pwait` applies `sigmask` with `pthread_sigmask()` around the wait,
  so it is not atomic. A signal that arrives just before the wait runs its
  handler but does not produce `EINTR`.

**eventfd, timerfd, signalfd**

- Blocking reads fail with `EINTR` when a signal handler runs, even with
  `SA_RESTART`.
- Closing a descriptor while another thread is blocked on it makes that call
  fail with `EBADF`. On Linux it would keep waiting.
- `eventfd` writes must be exactly 8 bytes, as on Linux 6.4 and later. Reads
  accept 8 bytes or more.

**timerfd**

- `CLOCK_BOOTTIME` is the same as `CLOCK_MONOTONIC`.
- `TFD_TIMER_CANCEL_ON_SET` is accepted but never reports `ECANCELED`.
- `CLOCK_REALTIME` timers sleep in monotonic intervals, so a clock jump is
  noticed only at the next wakeup.
- `epoll_wait` and `read` see an expiration from its deadline on. A plain
  `poll()` or `select()` on the descriptor sees it once the helper thread
  has run.

**signalfd**

- As on Linux, block the signals in every thread before creating the
  signalfd.
- Do not change the disposition of those signals while a signalfd for them
  is open.
- Signals directed at one specific thread (`pthread_kill`, `raise`) are not
  seen, because only the helper thread receives them.
- A signal is queued at most once per signalfd until it is read. That
  applies to real-time signals too, which Linux would queue repeatedly.
- Each signalfd whose mask contains a signal gets its own copy. On Linux,
  whichever signalfd reads first consumes it.
- `SIGKILL`, `SIGSTOP`, `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE` and `SIGTRAP`
  are dropped from masks.
- On Hurd, glibc does not report the sender, so `ssi_pid` and `ssi_uid` are
  `0xffffffff`.

**fork()**

- In the child, descriptors inherited from the parent support only
  `close()`. Everything else fails with `EBADF`, because their helper threads
  do not exist and their locks may be held.
- Objects created in the child work normally.
- Signal dispositions that the shim installed are restored in the child.

**Resource use**

- Each object uses 3 descriptors: the private pipe and the user descriptor.
  An epoll object uses 5.
- Each epoll object and each armed timerfd costs one thread.

## Build, test, install

```sh
make                        # libepoll-shim.so(.0) and libepoll-shim.a
make check                  # TAP tests against the shim
make check-kernel           # the same tests against Linux, as a control
make check-asan check-tsan  # sanitizer builds (Linux host)
make check-loop N=50        # repeat the timing-sensitive tests
make install PREFIX=/usr DESTDIR=/tmp/stage
make install-tests TEST_RPATH=/usr/lib PREFIX=/usr DESTDIR=/tmp/stage
```

`install-tests` installs the test programs as
`$(PREFIX)/bin/hurd-epoll-shim-test-NAME`, for running them where the
library is installed (Hurdkit runs them in its Guix GNU/Hurd guest).

`CC`, `CFLAGS`, `AR`, `PREFIX`, `LIBDIR`, `INCLUDEDIR` and `DESTDIR` are
honoured. `WERROR=` drops `-Werror`.

The headers install to `$(PREFIX)/include/hurd-epoll-shim`. The library
installs to `$(PREFIX)/lib`, together with `epoll-shim.pc`. That file
provides:

- `Cflags: -I${includedir}/hurd-epoll-shim`
- `Libs: -L${libdir} -lepoll-shim -lpthread`

## libwayland

libwayland finds this library with `dependency('epoll-shim')`, which is a
pkg-config lookup. However, its `meson.build` only does that when
`host_machine.system()` is `freebsd` or `openbsd`. For GNU/Hurd, add `'gnu'`
to that list in the port (a one-line change).

The rest needs no changes:

- `event-loop.c`, `wayland-os.c` and `wayland-server.c` already include the
  epoll, signalfd, timerfd and eventfd headers, so their direct `read`,
  `write` and `close` calls go through the wrappers.
- `wl_os_dupfd_cloexec()` calls libc `fcntl` through the `wl_fcntl` function
  pointer, so the macro does not apply there. That is harmless:
  `wl_event_loop_add_fd()` only polls that duplicate and closes it with a
  wrapped `close()`. A raw duplicate of a shim descriptor polls correctly,
  because readiness lives in the pipe. Only `read` and `write` on such a
  duplicate would not be emulated.

Put the installed `lib/pkgconfig` directory on `PKG_CONFIG_PATH`. The
`Cflags` place these headers ahead of the system ones.
