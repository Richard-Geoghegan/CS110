/* perf_daemon.c - Container-scoped PMU sampler with OTLP export & SLO-triggered frequency toggle */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <stdatomic.h>
#include <stdint.h>

#define MMAP_PAGES        8
#define BASELINE_FREQ     100
#define TRIGGERED_FREQ    4000
#define OTLP_ENDPOINT     "http://localhost:4318/v1/traces"
#define BATCH_SIZE        64
#define TIMEOUT_MS        500
#define CTL_SOCKET_PATH   "/tmp/perf_daemon_ctl"

static volatile sig_atomic_t running = 1;
static int perf_fd = -1;
static void *mmap_buf = NULL;
static int ctl_fd = -1;
static atomic_int current_freq = BASELINE_FREQ;
static const char *g_cgroup_path = NULL;

static void handle_sigint(int sig) { running = 0; }

/* Open perf event scoped to a cgroup v2 container */
static int open_perf_event(const char *cgroup_path, int freq) {
    int cg_fd = open(cgroup_path, O_RDONLY);
    if (cg_fd < 0) { perror("cgroup open"); return -1; }

    struct perf_event_attr pe = {0};
    pe.type = PERF_TYPE_HARDWARE;
    pe.config = PERF_COUNT_HW_CACHE_MISSES;
    pe.size = sizeof(pe);
    pe.freq = 1;           /* required: treat sample_period as a frequency */
    pe.sample_freq = freq;
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
    pe.disabled = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.use_clockid = 1;
    pe.clockid = CLOCK_MONOTONIC;

    /* cgroup-scoped sampling: pass cgroup fd as pid with PERF_FLAG_PID_CGROUP */
    int fd = syscall(SYS_perf_event_open, &pe, cg_fd, 0, -1,
                     PERF_FLAG_FD_CLOEXEC | PERF_FLAG_PID_CGROUP);
    close(cg_fd);
    return fd;
}

/* SLO-triggered frequency toggle */
static int toggle_sampling_freq(int new_freq) {
    if (new_freq == atomic_load(&current_freq)) return 0;
    printf("[DAEMON] Toggling sampling: %d Hz -> %d Hz\n", atomic_load(&current_freq), new_freq);
    
    /* Recreate fd with new frequency (safest for live tuning) */
    if (perf_fd >= 0) close(perf_fd);
    perf_fd = open_perf_event(g_cgroup_path, new_freq);
    if (perf_fd < 0) return -1;

    /* Re-map ring buffer */
    if (mmap_buf) munmap(mmap_buf, (MMAP_PAGES + 1) * sysconf(_SC_PAGESIZE));
    int page_size = sysconf(_SC_PAGESIZE);
    mmap_buf = mmap(NULL, (MMAP_PAGES + 1) * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);
    if (mmap_buf == MAP_FAILED) { perror("mmap"); return -1; }

    atomic_store(&current_freq, new_freq);
    return 0;
}

/* Fetch trace context from app-side Unix socket */
static int fetch_trace_context(pid_t tid, char *trace_id, char *span_id, size_t buf_size) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/trace_bridge.sock");
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return -1; }

    char req[64];
    snprintf(req, sizeof(req), "QUERY %d\n", tid);
    send(sock, req, strlen(req), 0);

    char resp[256];
    ssize_t n = recv(sock, resp, sizeof(resp)-1, 0);
    resp[n] = '\0';
    close(sock);

    if (sscanf(resp, "%s %s", trace_id, span_id) == 2) return 0;
    return -1;
}

/* Batch OTLP JSON export */
static int export_to_otel(const char *json_payload) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
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

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <cgroup.procs path> [freq_hz]\n", argv[0]); return 1; }
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    curl_global_init(CURL_GLOBAL_ALL);

    g_cgroup_path = argv[1];
    int start_freq = (argc >= 3) ? atoi(argv[2]) : BASELINE_FREQ;
    atomic_store(&current_freq, start_freq);
    printf("[DAEMON] Starting at %d Hz\n", start_freq);

    /* Initialize perf & mmap */
    perf_fd = open_perf_event(g_cgroup_path, start_freq);
    if (perf_fd < 0) return 1;
    int page_size = sysconf(_SC_PAGESIZE);
    mmap_buf = mmap(NULL, (MMAP_PAGES + 1) * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);
    if (mmap_buf == MAP_FAILED) { perror("mmap"); return 1; }
    struct perf_event_mmap_page *header = mmap_buf;

    /* Control socket for SLO trigger */
    ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ctl_addr = {.sun_family = AF_UNIX, .sun_path = CTL_SOCKET_PATH};
    unlink(CTL_SOCKET_PATH);
    bind(ctl_fd, (struct sockaddr*)&ctl_addr, sizeof(ctl_addr));
    listen(ctl_fd, 5);

    printf("[DAEMON] Running baseline sampling @ %d Hz\n", BASELINE_FREQ);

    while (running) {
        /* Check control socket for SLO trigger */
        fd_set rfds;
        FD_ZERO(&rfds); FD_SET(ctl_fd, &rfds);
        struct timeval tv = {0, 50000}; // 50ms timeout
        if (select(ctl_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
            int conn = accept(ctl_fd, NULL, NULL);
            if (conn >= 0) {
                char cmd[16]; read(conn, cmd, sizeof(cmd));
                if (strncmp(cmd, "TRIGGER_HIGH", 12) == 0) toggle_sampling_freq(TRIGGERED_FREQ);
                else if (strncmp(cmd, "REVERT_BASE", 11) == 0) toggle_sampling_freq(BASELINE_FREQ);
                close(conn);
            }
        }

        /* Read ring buffer */
        __sync_synchronize();
        uint64_t head = header->data_head;
        uint64_t tail = header->data_tail;
        char *data = mmap_buf + page_size;

        while (tail < head) {
            struct perf_event_header *hdr = (void*)(data + (tail % (MMAP_PAGES * page_size)));
            if (hdr->type == PERF_RECORD_SAMPLE) {
                /* Layout matches sample_type: IP | TID | TIME */
                struct { uint64_t ip; uint32_t pid; uint32_t tid; uint64_t time; } *s =
                    (void*)(hdr + 1);
                pid_t tid = (pid_t)s->tid;
                char trace_id[64] = "unknown", span_id[64] = "unknown";
                fetch_trace_context(tid, trace_id, span_id, sizeof(trace_id));

                /* Build OTLP span attribute payload */
                char batch_json[2048];
                snprintf(batch_json, sizeof(batch_json),
                    "{\"resourceSpans\":[{\"resource\":{},\"scopeSpans\":[{\"spans\":[{\"traceId\":\"%s\",\"spanId\":\"%s\",\"name\":\"pmu_sample\",\"attributes\":[{\"key\":\"pmu.event\",\"value\":{\"stringValue\":\"cache-misses\"}},{\"key\":\"tid\",\"value\":{\"intValue\":%d}}]}]}]}]}",
                    trace_id, span_id, tid);
                
                export_to_otel(batch_json);
            }
            tail += hdr->size;
        }
        __sync_synchronize();
        header->data_tail = head;
        usleep(10000); // 10ms poll
    }

    /* Cleanup */
    if (mmap_buf) munmap(mmap_buf, (MMAP_PAGES + 1) * page_size);
    if (perf_fd >= 0) close(perf_fd);
    close(ctl_fd);
    curl_global_cleanup();
    printf("[DAEMON] Shutdown complete.\n");
    return 0;
}
