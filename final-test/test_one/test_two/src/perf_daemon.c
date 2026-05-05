#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L  /* Guarantees CLOCK_MONOTONIC & syscall visibility */
#include <unistd.h>              /* Declares syscall() */
#include <time.h>                /* Defines CLOCK_MONOTONIC */
#include <sys/syscall.h>         /* Defines SYS_perf_event_open */
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
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

// P3
#define TRIGGER_WINDOW_SEC 15
static time_t last_trigger_time = 0;
static bool is_in_high_freq_mode = false;

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
    pe.type = PERF_TYPE_HARDWARE;
    pe.config = PERF_COUNT_HW_CACHE_MISSES;
    pe.size = sizeof(pe);
    pe.sample_freq = freq;
    /* IP | TIME | TID | ID layout for ring buffer parsing */
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TIME | PERF_SAMPLE_TID | PERF_SAMPLE_ID;
    pe.disabled = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.use_clockid = 1;
    pe.clockid = CLOCK_MONOTONIC;
    pe.cgroup = cg_fd;
    pe.wakeup_events = BATCH_SIZE;

    int fd = syscall(SYS_perf_event_open, &pe, -1, 0, -1, PERF_FLAG_FD_CLOEXEC);
    close(cg_fd);
    return fd;
}

/* SLO-triggered frequency toggle */
static int toggle_sampling_freq(int new_freq) {
    if (new_freq == atomic_load(&current_freq)) return 0;
    printf("[DAEMON] ⚡ Toggling: %d Hz → %d Hz\n", atomic_load(&current_freq), new_freq);

    if (perf_fd >= 0) {
        ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0);
        close(perf_fd);
    }

    perf_fd = open_perf_event(TARGET_CGROUP, new_freq);
    if (perf_fd < 0) return -1;

    int page_size = sysconf(_SC_PAGESIZE);
    if (mmap_buf) munmap(mmap_buf, (MMAP_PAGES + 1) * page_size);
    mmap_buf = mmap(NULL, (MMAP_PAGES + 1) * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);
    if (mmap_buf == MAP_FAILED) { perror("mmap"); return -1; }

    atomic_store(&current_freq, new_freq);

    if (new_freq == TRIGGERED_FREQ) {
        last_trigger_time = time(NULL);
        is_in_high_freq_mode = true;
    } else if (new_freq == BASELINE_FREQ) {
        is_in_high_freq_mode = false;
    }

    return 0;
}

/* Fetch trace context from app-side Unix socket */
static int fetch_trace_context(pid_t tid, char *trace_id, char *span_id, size_t buf_size) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), TRACE_SOCK_PATH);
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return -1; }

    char req[32];
    int len = snprintf(req, sizeof(req), "QUERY %d\n", tid);
    if (send(sock, req, len, 0) < 0) { close(sock); return -1; }



    char resp[128];
    ssize_t n = recv(sock, resp, sizeof(resp)-1, 0);
    if (n < 0) { close(sock); return -1; }
    resp[n] = '\0';
    close(sock);

    /* Width specifiers prevent buffer overrun */
    if (sscanf(resp, "%63s %63s", trace_id, span_id) == 2) return 0;
    (void)buf_size; /* Acknowledge parameter */
    return -1;
}

/* Discard OTLP HTTP response to keep terminal clean */
static size_t curl_discard_cb(void *ptr, size_t size, size_t nmemb, void *stream) {
    (void)ptr; (void)stream;
    return size * nmemb;
}

/* Batch OTLP JSON export */
static int export_to_otel(const char *json_payload) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discard_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(curl, CURLOPT_URL, OTLP_ENDPOINT);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TIMEOUT_MS);
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : -1;
}

static void gen_hex_id(char *buf, size_t chars) {
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < chars; i++) buf[i] = hex[rand() % 16];
    buf[chars] = '\0';
}

int main(int argc, char *argv[]) {

    if (argc < 2) {
        fprintf(stderr, "Usage: sudo %s <cgroup_procs_path>\n", argv[0]);
        return 1;
    }

    TARGET_CGROUP = argv[1];

    (void)argc; (void)argv;
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    curl_global_init(CURL_GLOBAL_ALL);

    perf_fd = open_perf_event(TARGET_CGROUP, BASELINE_FREQ);
    if (perf_fd < 0) return 1;
    int page_size = sysconf(_SC_PAGESIZE);
    mmap_buf = mmap(NULL, (MMAP_PAGES + 1) * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);
    if (mmap_buf == MAP_FAILED) { perror("mmap"); return 1; }
    struct perf_event_mmap_page *header = mmap_buf;

    ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ctl_addr = {.sun_family = AF_UNIX, .sun_path = CTL_SOCKET_PATH};
    unlink(CTL_SOCKET_PATH);
    bind(ctl_fd, (struct sockaddr*)&ctl_addr, sizeof(ctl_addr));
    listen(ctl_fd, 5);
    printf("[DAEMON] 🟢 Baseline @ %d Hz | Control: %s\n", BASELINE_FREQ, CTL_SOCKET_PATH);

    char batch_json[4096];
    int batch_idx = 0;

    while (running) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(ctl_fd, &rfds);
        struct timeval tv = {0, 100000}; // 100ms poll
        if (select(ctl_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
            int conn = accept(ctl_fd, NULL, NULL);
            if (conn >= 0) {
                char cmd[32] = {0};
                ssize_t n = read(conn, cmd, sizeof(cmd)-1);
                if (n > 0) {
                    cmd[n] = '\0';
                    if (strstr(cmd, "TRIGGER_HIGH")) toggle_sampling_freq(TRIGGERED_FREQ);
                    else if (strstr(cmd, "REVERT_BASE")) toggle_sampling_freq(BASELINE_FREQ);
                }
                close(conn);
            }
        }

        // --- Phase 4: Auto-revert check for SLO-triggered window ---
        if (is_in_high_freq_mode) {
            time_t elapsed = time(NULL) - last_trigger_time;
            if (elapsed >= TRIGGER_WINDOW_SEC) {
                printf("[DAEMON] Trigger window expired (%lds). Auto-reverting...\n", elapsed);
                toggle_sampling_freq(BASELINE_FREQ);
            }
        }

        __sync_synchronize();
        uint64_t head = header->data_head;
        uint64_t tail = header->data_tail;
        char *data = (char*)mmap_buf + page_size;

        while (tail < head) {
            struct perf_event_header *hdr = (struct perf_event_header*)(data + (tail % (MMAP_PAGES * page_size)));
            if (hdr->type == PERF_RECORD_SAMPLE) {
                /* Manual parsing of ring buffer based on: IP | TIME | TID | ID */
                uint8_t *ptr = (uint8_t*)(hdr + 1);
                // uint64_t ip = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
                uint64_t time_ns = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
                uint32_t pid = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
                uint32_t tid = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
                uint64_t id = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
                (void)time_ns; (void)pid; (void)id; /* Keep compiler happy */

                // printf("Captured cache miss for TID: %u\n", tid); // Debug print

                char trace_id[64] = "00000000000000000000000000000000";
                char span_id[64] = "0000000000000000";
                fetch_trace_context((pid_t)tid, trace_id, span_id, sizeof(trace_id));

                /* Fallback to valid OTel IDs if bridge returns empty/short strings */
                if (strlen(trace_id) != 32) gen_hex_id(trace_id, 32);
                if (strlen(span_id) != 16) gen_hex_id(span_id, 16);

                /* Capture precise timestamps (OTLP requires Unix Nano as strings) */
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t start_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
                uint64_t end_ns = start_ns + 50000; // ~50µs span duration
                // Phase 3: OTel-compliant schema (traceId/spanId top-level, semantic attributes)
    //             batch_idx += snprintf(batch_json + batch_idx, sizeof(batch_json) - batch_idx,
    // "{\"traceId\":\"%s\",\"spanId\":\"%s\",\"name\":\"pmu.cache_miss\",\"kind\":3,\"attributes\":[{\"key\":\"thread.id\",\"value\":{\"intValue\":%u}},{\"key\":\"pmu.event\",\"value\":{\"stringValue\":\"cache-misses\"}}]},",
    // trace_id, span_id, tid);
                batch_idx += snprintf(batch_json + batch_idx, sizeof(batch_json) - batch_idx,
    "{\"traceId\":\"%s\",\"spanId\":\"%s\",\"parentSpanId\":\"\",\"name\":\"pmu_cache_miss\","
    "\"kind\":1,\"startTimeUnixNano\":\"%lu\",\"endTimeUnixNano\":\"%lu\","
    "\"attributes\":[{\"key\":\"tid\",\"value\":{\"intValue\":%u}},{\"key\":\"pmu.event\",\"value\":{\"stringValue\":\"cache-misses\"}}]},",
    trace_id, span_id, (unsigned long)start_ns, (unsigned long)end_ns, tid);

                if (batch_idx > 3000) {
                    char payload[4096];
                    // Edit from G
                    snprintf(payload, sizeof(payload), 
    "{\"resourceSpans\":[{\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":\"perf-daemon\"}}]},\"scopeSpans\":[{\"spans\":[%.*s]}]}]}", 
    batch_idx-1, batch_json);

                    export_to_otel(payload);
                    batch_idx = 0;
                }
            }
            tail += hdr->size;
        }
        __sync_synchronize();
        header->data_tail = head;
    }
    
    // Prevent tight CPU polling
    usleep(10000);

    if (mmap_buf) munmap(mmap_buf, (MMAP_PAGES + 1) * page_size);
    if (perf_fd >= 0) close(perf_fd);
    close(ctl_fd);
    curl_global_cleanup();
    return 0;
}
