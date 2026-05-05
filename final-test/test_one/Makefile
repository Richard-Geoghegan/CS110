CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lcurl -lpthread

all: perf_daemon

perf_daemon: perf_daemon.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f perf_daemon

install: perf_daemon
	sudo setcap cap_sys_admin,cap_perfmon+ep perf_daemon
	sudo cp perf_daemon /usr/local/bin/

.PHONY: all clean install
