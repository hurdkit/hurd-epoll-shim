# SPDX-License-Identifier: MIT
# Copyright (c) 2026 7778777

VERSION = 0.1.0
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
DESTDIR ?=
CFLAGS ?= -O2 -g
WERROR ?= -Werror
N ?= 50
# The run path of the installed test programs (install-tests).
TEST_RPATH ?= $(CURDIR)

STD = -std=c11 -Wall -Wextra $(WERROR)
INC = -Iinclude/hurd-epoll-shim
SRCS = src/core.c src/epoll.c src/eventfd.c src/signalfd.c src/timerfd.c
OBJS = $(SRCS:src/%.c=build/%.o)
HDRS = include/hurd-epoll-shim/epoll-shim/wrap.h \
	include/hurd-epoll-shim/sys/epoll.h include/hurd-epoll-shim/sys/eventfd.h \
	include/hurd-epoll-shim/sys/signalfd.h include/hurd-epoll-shim/sys/timerfd.h
TESTS = t_eventfd t_timerfd t_signalfd t_epoll t_misc t_stress t_idle
ASAN_TESTS = t_eventfd t_timerfd t_signalfd t_epoll t_misc t_stress
TSAN_TESTS = t_eventfd t_timerfd t_signalfd t_epoll t_misc t_stress
LOOP_TESTS = t_eventfd t_timerfd t_signalfd t_epoll t_misc
SONAME = libepoll-shim.so.0
SAN = -O1 -g -fno-omit-frame-pointer -pthread

all: libepoll-shim.so libepoll-shim.a

build/%.o: src/%.c src/shim.h $(HDRS)
	@mkdir -p build
	$(CC) $(STD) $(INC) -fPIC -fvisibility=hidden -pthread $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(SONAME): $(OBJS)
	$(CC) -shared -Wl,-soname,$(SONAME) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) -pthread

libepoll-shim.so: $(SONAME)
	ln -sf $(SONAME) $@

libepoll-shim.a: $(OBJS)
	rm -f $@
	$(AR) rcs $@ $(OBJS)

epoll-shim.pc: Makefile
	printf '%s\n' 'prefix=$(PREFIX)' 'includedir=$(INCLUDEDIR)' \
	    'libdir=$(LIBDIR)' '' 'Name: epoll-shim' \
	    'Description: epoll, eventfd, timerfd and signalfd for GNU/Hurd' \
	    'Version: $(VERSION)' 'Cflags: -I$${includedir}/hurd-epoll-shim' \
	    'Libs: -L$${libdir} -lepoll-shim -lpthread' > $@

# Tests: shim build, kernel control build, sanitizer builds.
build/shim/%: tests/%.c tests/tap.h libepoll-shim.so $(HDRS)
	@mkdir -p build/shim
	$(CC) $(STD) $(INC) $(CFLAGS) $< -o $@ -L. -lepoll-shim \
	    -Wl,-rpath,$(TEST_RPATH) -pthread

build/kernel/%: tests/%.c tests/tap.h
	@mkdir -p build/kernel
	$(CC) $(STD) $(CFLAGS) $< -o $@ -pthread

build/asan/%: tests/%.c tests/tap.h $(SRCS) src/shim.h $(HDRS)
	@mkdir -p build/asan
	$(CC) $(STD) $(INC) $(SAN) -fsanitize=address,undefined \
	    -fno-sanitize-recover=all $(SRCS) $< -o $@

build/tsan/%: tests/%.c tests/tap.h $(SRCS) src/shim.h $(HDRS)
	@mkdir -p build/tsan
	$(CC) $(STD) $(INC) $(SAN) -fsanitize=thread $(SRCS) $< -o $@

check: $(TESTS:%=build/shim/%)
	sh tests/run.sh $^

check-kernel: $(TESTS:%=build/kernel/%)
	sh tests/run.sh $^

check-asan: $(ASAN_TESTS:%=build/asan/%)
	ASAN_OPTIONS=detect_leaks=1 sh tests/run.sh $^

check-tsan: $(TSAN_TESTS:%=build/tsan/%)
	TSAN_OPTIONS=halt_on_error=1 sh tests/run.sh $^

check-loop: $(LOOP_TESTS:%=build/shim/%)
	@i=0; fails=0; \
	while [ $$i -lt $(N) ]; do \
	    i=$$((i + 1)); \
	    if ! QUIET=1 sh tests/run.sh $^ > build/loop.log 2>&1; then \
	        fails=$$((fails + 1)); echo "iteration $$i:"; \
	        grep -A8 FAIL build/loop.log; \
	    fi; \
	done; \
	echo "check-loop: $$fails of $(N) iterations failed"; \
	test $$fails -eq 0

install: all epoll-shim.pc
	install -d $(DESTDIR)$(INCLUDEDIR)/hurd-epoll-shim/sys \
	    $(DESTDIR)$(INCLUDEDIR)/hurd-epoll-shim/epoll-shim \
	    $(DESTDIR)$(LIBDIR)/pkgconfig
	install -m 644 include/hurd-epoll-shim/sys/*.h \
	    $(DESTDIR)$(INCLUDEDIR)/hurd-epoll-shim/sys/
	install -m 644 include/hurd-epoll-shim/epoll-shim/wrap.h \
	    $(DESTDIR)$(INCLUDEDIR)/hurd-epoll-shim/epoll-shim/
	install -m 755 $(SONAME) $(DESTDIR)$(LIBDIR)/
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libepoll-shim.so
	install -m 644 libepoll-shim.a $(DESTDIR)$(LIBDIR)/
	install -m 644 epoll-shim.pc $(DESTDIR)$(LIBDIR)/pkgconfig/

# The test programs as hurd-epoll-shim-test-NAME in $(PREFIX)/bin, to run
# where the library is installed. Build them with TEST_RPATH=$(LIBDIR).
install-tests: $(TESTS:%=build/shim/%)
	install -d $(DESTDIR)$(PREFIX)/bin
	for t in $(TESTS); do \
	    install -m 755 build/shim/$$t \
	        $(DESTDIR)$(PREFIX)/bin/hurd-epoll-shim-test-$${t#t_}; \
	done

clean:
	rm -rf build libepoll-shim.so $(SONAME) libepoll-shim.a epoll-shim.pc

.PHONY: all check check-kernel check-asan check-tsan check-loop install install-tests clean
.PRECIOUS: build/shim/% build/kernel/% build/asan/% build/tsan/%
