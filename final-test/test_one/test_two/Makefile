CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
LDFLAGS = -lcurl -lpthread
SRC = src/perf_daemon.c
OUT = bin/perf_daemon

.PHONY: all clean install deps

all: deps $(OUT)

deps:
	@command -v curl-config >/dev/null 2>&1 || \
	{ echo "❌ Missing libcurl-dev. Run: sudo apt install libcurl4-openssl-dev"; exit 1; }

$(OUT): $(SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -rf bin

install: $(OUT)
	sudo setcap cap_perfmon,cap_sys_admin+ep $(OUT)
	sudo cp $(OUT) /usr/local/bin/
