#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L  /* Guarantees CLOCK_REALTIME & syscall visibility */
#include <unistd.h>
#include <time.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>

#define MMAP_PAGES        8
#define BASELINE_FREQ     100
#define TRIGGERED_FREQ    4000
#define OTLP_ENDPOINT     "http://localhost:4318/v1/traces"
#define BATCH_SIZE        64
#define TIMEOUT_MS        500
#define CTL_SOCKET_PATH   "/tmp/perf_daemon_ctl"
#define TRACE_SOCK_PATH   "/tmp/trace_bridge.sock"

/*
 * Each span JSON is ~280 bytes. BATCH_SIZE=64 → max ~18 KB.
 * Use a 32 KB batch buffer and flush when > 24 KB to stay safe.
 */
#define BATCH_BUF_SIZE    (32 * 1024)
#define BATCH_FLUSH_MARK  (24 * 1024)

static volatile sig_atomic_t running = 1;
static int perf_fd = -1;
static void *mmap_buf = NULL;
static int ctl_fd = -1;
static atomic_int current_freq = BASELINE_FREQ;
static const char *TARGET_CGROUP = NULL;

static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

/* Open perf event scoped to cgroup v2 container */
static int open_perf_event(const char *cgroup_path, int freq) {
    int cg_fd = open(cgroup_path, O_RDONLY);
    if (cg_fd < 0) { perror("cgroup open"); return -1; }

    struct perf_event_attr pe = {0};
    pe.type           = PERF_TYPE_HARDWARE;
    pe.config         = PERF_COUNT_HW_CACHE_MISSES;
    pe.size           = sizeof(pe);
    pe.sample_freq    = freq;
    pe.freq           = 1;          /* sample_freq is Hz, not period */
    /*
     * Field order in the ring buffer record matches the bit order of
     * PERF_SAMPLE_* flags (lowest bit first):
     *   IP(bit0) | TIME(bit2) | TID(bit3) | ID(bit6)
     * All four must be parsed in that order; IP cannot be skipped by
     * simply not advancing the pointer.
     */
    pe.sample_type    = PERF_SAMPLE_IP | PERF_SAMPLE_TIME |
                        PERF_SAMPLE_TID | PERF_SAMPLE_ID;
    pe.disabled       = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;
    /*
     * Use CLOCK_REALTIME so pe.TIME values are wall-clock nanoseconds
     * and can be used directly as OTLP startTimeUnixNano if desired.
     * (We still take a fresh CLOCK_REALTIME sample per-span below for
     * simplicity, but the clockid must be consistent.)
     */
    pe.use_clockid    = 1;
    pe.clockid        = CLOCK_REALTIME;
    pe.cgroup         = cg_fd;
    pe.wakeup_events  = BATCH_SIZE;

    int fd = syscall(SYS_perf_event_open, &pe, -1, 0, -1, PERF_FLAG_FD_CLOEXEC);
    close(cg_fd);
    return fd;
}

/* SLO-triggered frequency toggle */
static int toggle_sampling_freq(int new_freq) {
    if (new_freq == atomic_load(&current_freq)) return 0;
    printf("[DAEMON] ⚡ Toggling: %d Hz → %d Hz\n",
           atomic_load(&current_freq), new_freq);

    if (perf_fd >= 0) {
        ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0);
        close(perf_fd);
    }

    perf_fd = open_perf_event(TARGET_CGROUP, new_freq);
    if (perf_fd < 0) return -1;

    int page_size = sysconf(_SC_PAGESIZE);
    if (mmap_buf) munmap(mmap_buf, (MMAP_PAGES + 1) * page_size);
    mmap_buf = mmap(NULL, (MMAP_PAGES + 1) * page_size,
                    PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);
    if (mmap_buf == MAP_FAILED) { perror("mmap"); return -1; }

    atomic_store(&current_freq, new_freq);
    return 0;
}

/*
 * Fetch trace context from app-side Unix socket.
 *
 * trace_id must be at least TRACE_ID_LEN+1 bytes (33).
 * span_id  must be at least SPAN_ID_LEN+1  bytes (17).
 * The OTel wire lengths are fixed constants, so we use them directly
 * rather than a shared buf_size that the caller could get wrong.
 */
#define TRACE_ID_LEN 32   /* 128-bit trace-id as lowercase hex */
#define SPAN_ID_LEN  16   /* 64-bit  span-id  as lowercase hex */

static int fetch_trace_context(pid_t tid,
                                char *trace_id,   /* out: TRACE_ID_LEN+1 */
                                char *span_id) {  /* out: SPAN_ID_LEN+1  */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", TRACE_SOCK_PATH);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); return -1;
    }

    char req[32];
    int len = snprintf(req, sizeof(req), "QUERY %d\n", tid);
    if (send(sock, req, len, 0) < 0) { close(sock); return -1; }

    char resp[256];
    ssize_t n = recv(sock, resp, sizeof(resp) - 1, 0);
    close(sock);
    if (n <= 0) return -1;
    resp[n] = '\0';

    /*
     * Bridge returns "<32-char-hex-trace-id> <16-char-hex-span-id>".
     * Parse into correctly-sized local buffers first, then copy with
     * bounds that match the destination (not the source).
     */
    char t[TRACE_ID_LEN + 1];
    char s[SPAN_ID_LEN  + 1];
    if (sscanf(resp, "%32s %16s", t, s) == 2 &&
        strlen(t) == TRACE_ID_LEN && strlen(s) == SPAN_ID_LEN) {
        snprintf(trace_id, TRACE_ID_LEN + 1, "%s", t);
        snprintf(span_id,  SPAN_ID_LEN  + 1, "%s", s);
        return 0;
    }
    return -1;
}

/* Discard OTLP HTTP response */
static size_t curl_discard_cb(void *ptr, size_t size, size_t nmemb,
                               void *stream) {
    (void)ptr; (void)stream;
    return size * nmemb;
}

/* Batch OTLP JSON export */
static int export_to_otel(const char *json_payload) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *headers =
        curl_slist_append(NULL, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discard_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(curl, CURLOPT_URL, OTLP_ENDPOINT);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : -1;
}

/*
 * Generate a cryptographically-weak but unique hex ID.
 * Uses /dev/urandom so IDs are not predictable across restarts.
 * `chars` must be even; buf must hold chars+1 bytes.
 */
static void gen_hex_id(char *buf, size_t chars) {
    static int urandom_fd = -1;
    if (urandom_fd < 0) urandom_fd = open("/dev/urandom", O_RDONLY);

    uint8_t raw[16];   /* enough for a 32-char (128-bit) trace ID */
    size_t bytes = chars / 2;
    if (bytes > sizeof(raw)) bytes = sizeof(raw);

    if (urandom_fd >= 0 && read(urandom_fd, raw, bytes) == (ssize_t)bytes) {
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < bytes; i++) {
            buf[i * 2]     = hex[raw[i] >> 4];
            buf[i * 2 + 1] = hex[raw[i] & 0xf];
        }
    } else {
        /* Last-resort fallback */
        const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < chars; i++) buf[i] = hex[rand() % 16];
    }
    buf[chars] = '\0';
}

/* Flush the accumulated span batch to the OTLP collector. */
static void flush_batch(char *batch_json, int *batch_idx) {
    if (*batch_idx <= 0) return;

    /*
     * Payload size = wrapper (~200 B) + spans.
     * Allocate on heap to avoid large stack frames.
     */
    size_t payload_size = (size_t)*batch_idx + 256;
    char *payload = malloc(payload_size);
    if (!payload) { *batch_idx = 0; return; }

    /*
     * Strip the trailing comma left by the last snprintf, then wrap in
     * the OTLP resourceSpans envelope.
     */
    snprintf(payload, payload_size,
        "{"
          "\"resourceSpans\":[{"
            "\"resource\":{"
              "\"attributes\":[{"
                "\"key\":\"service.name\","
                "\"value\":{\"stringValue\":\"perf-daemon\"}"
              "}]"
            "},"
            "\"scopeSpans\":[{"
              "\"scope\":{\"name\":\"perf_daemon\",\"version\":\"1.0.0\"},"
              "\"spans\":[%.*s]"   /* %.*s trims the trailing comma */
            "}]"
          "}]"
        "}",
        *batch_idx - 1, batch_json);   /* -1 drops the final ',' */

    export_to_otel(payload);
    free(payload);
    *batch_idx = 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: sudo %s <cgroup_procs_path>\n", argv[0]);
        return 1;
    }
    TARGET_CGROUP = argv[1];

    signal(SIGINT,  handle_sigint);
    signal(SIGTERM, handle_sigint);
    curl_global_init(CURL_GLOBAL_ALL);

    perf_fd = open_perf_event(TARGET_CGROUP, BASELINE_FREQ);
    if (perf_fd < 0) return 1;

    int page_size = sysconf(_SC_PAGESIZE);
    mmap_buf = mmap(NULL, (MMAP_PAGES + 1) * page_size,
                    PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);
    if (mmap_buf == MAP_FAILED) { perror("mmap"); return 1; }
    struct perf_event_mmap_page *header = mmap_buf;

    /* Control socket */
    ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ctl_addr = {.sun_family = AF_UNIX};
    snprintf(ctl_addr.sun_path, sizeof(ctl_addr.sun_path), "%s", CTL_SOCKET_PATH);
    unlink(CTL_SOCKET_PATH);
    bind(ctl_fd, (struct sockaddr*)&ctl_addr, sizeof(ctl_addr));
    listen(ctl_fd, 5);
    printf("[DAEMON] 🟢 Baseline @ %d Hz | Control: %s\n",
           BASELINE_FREQ, CTL_SOCKET_PATH);

    char *batch_json = malloc(BATCH_BUF_SIZE);
    if (!batch_json) { perror("malloc"); return 1; }
    int batch_idx = 0;

    while (running) {
        /* Poll control socket for frequency-change commands (100 ms) */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ctl_fd, &rfds);
        struct timeval tv = {0, 100000};
        if (select(ctl_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
            int conn = accept(ctl_fd, NULL, NULL);
            if (conn >= 0) {
                char cmd[32] = {0};
                ssize_t n = read(conn, cmd, sizeof(cmd) - 1);
                if (n > 0) {
                    cmd[n] = '\0';
                    if      (strstr(cmd, "TRIGGER_HIGH")) toggle_sampling_freq(TRIGGERED_FREQ);
                    else if (strstr(cmd, "REVERT_BASE"))  toggle_sampling_freq(BASELINE_FREQ);
                }
                close(conn);
            }
        }

        /* --- Drain the perf ring buffer --- */
        __sync_synchronize();   /* read barrier before inspecting head */
        uint64_t head = header->data_head;
        uint64_t tail = header->data_tail;
        char *data    = (char*)mmap_buf + page_size;
        size_t ring   = (size_t)(MMAP_PAGES * page_size);

        while (tail < head) {
            struct perf_event_header *hdr =
                (struct perf_event_header*)(data + (tail % ring));

            if (hdr->size == 0) break;   /* sanity: avoid infinite loop */

            if (hdr->type == PERF_RECORD_SAMPLE) {
                /*
                 * Field layout matches sample_type bit order (LSB first):
                 *   [0] PERF_SAMPLE_IP    → uint64_t ip
                 *   [2] PERF_SAMPLE_TIME  → uint64_t time (CLOCK_REALTIME ns)
                 *   [3] PERF_SAMPLE_TID   → uint32_t pid, uint32_t tid
                 *   [6] PERF_SAMPLE_ID    → uint64_t id
                 *
                 * BUG FIX: the original code skipped advancing past IP,
                 * so every subsequent field was reading 8 bytes too early.
                 */
                uint8_t *ptr = (uint8_t*)(hdr + 1);

                uint64_t ip      = *(uint64_t*)ptr; ptr += 8; (void)ip;
                uint64_t time_ns = *(uint64_t*)ptr; ptr += 8;
                uint32_t pid     = *(uint32_t*)ptr; ptr += 4; (void)pid;
                uint32_t tid     = *(uint32_t*)ptr; ptr += 4;
                uint64_t id      = *(uint64_t*)ptr; ptr += 8; (void)id;

                /*
                 * Use the PMU timestamp (CLOCK_REALTIME, set via pe.clockid)
                 * as the span start time — this is the actual moment the
                 * cache miss occurred, not when we processed the record.
                 *
                 * BUG FIX (original): CLOCK_MONOTONIC was used, yielding
                 * seconds-since-boot rather than Unix epoch nanoseconds,
                 * which caused Jaeger to display "1970-01-01 00:00:00".
                 */
                uint64_t start_ns = time_ns;
                uint64_t end_ns   = start_ns + 50000ULL;  /* ~50 µs synthetic duration */

                /* Try to attach to an in-flight application trace */
                char trace_id[TRACE_ID_LEN + 1];
                char span_id [SPAN_ID_LEN  + 1];
                memset(trace_id, 0, sizeof(trace_id));
                memset(span_id,  0, sizeof(span_id));
                if (fetch_trace_context((pid_t)tid, trace_id, span_id) != 0) {
                    /* Bridge unavailable — generate standalone valid IDs */
                    gen_hex_id(trace_id, 32);
                    gen_hex_id(span_id,  16);
                }

                /*
                 * OTLP HTTP/JSON span object.
                 * - traceId:  32 lowercase hex chars (128-bit)
                 * - spanId:   16 lowercase hex chars (64-bit)
                 * - parentSpanId omitted (not "") when there is no parent;
                 *   an empty string is technically valid but some UIs warn.
                 * - kind 2 = INTERNAL (appropriate for a PMU hardware sample)
                 * - startTimeUnixNano / endTimeUnixNano are strings per proto3
                 *   JSON mapping (uint64 → string to avoid JS precision loss).
                 * - thread.id uses the OTel semantic convention key name.
                 */
                int written = snprintf(
                    batch_json + batch_idx,
                    BATCH_BUF_SIZE - batch_idx,
                    "{"
                      "\"traceId\":\"%s\","
                      "\"spanId\":\"%s\","
                      "\"name\":\"pmu.cache_miss\","
                      "\"kind\":2,"
                      "\"startTimeUnixNano\":\"%lu\","
                      "\"endTimeUnixNano\":\"%lu\","
                      "\"attributes\":["
                        "{\"key\":\"thread.id\","
                         "\"value\":{\"intValue\":%u}},"
                        "{\"key\":\"pmu.event\","
                         "\"value\":{\"stringValue\":\"cache-misses\"}},"
                        "{\"key\":\"pmu.ip\","
                         "\"value\":{\"intValue\":%lu}}"
                      "]"
                    "},",
                    trace_id, span_id,
                    (unsigned long)start_ns,
                    (unsigned long)end_ns,
                    tid,
                    (unsigned long)ip);

                if (written > 0) batch_idx += written;

                /* Flush before the buffer gets too full */
                if (batch_idx >= BATCH_FLUSH_MARK) {
                    flush_batch(batch_json, &batch_idx);
                }
            }

            tail += hdr->size;
        }

        __sync_synchronize();   /* write barrier before updating tail */
        header->data_tail = head;
    }

    /* Final flush of any buffered spans */
    flush_batch(batch_json, &batch_idx);

    free(batch_json);
    if (mmap_buf) munmap(mmap_buf, (MMAP_PAGES + 1) * page_size);
    if (perf_fd >= 0) close(perf_fd);
    close(ctl_fd);
    unlink(CTL_SOCKET_PATH);
    curl_global_cleanup();
    return 0;
}
