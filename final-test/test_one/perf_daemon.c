/* perf_daemon.c - PID-list PMU sampler with OTLP export */
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
#include <dirent.h>

#define MMAP_PAGES        16
#define MAX_PIDS          256
#define OTLP_ENDPOINT     "http://localhost:4318/v1/traces"
#define TIMEOUT_MS        500

static volatile sig_atomic_t running = 1;
static void handle_sigint(int sig) { (void)sig; running = 0; }

/* One perf fd + mmap per tracked PID */
typedef struct {
    pid_t  pid;
    int    fd;
    void  *buf;
} tracked_t;

static tracked_t tracked[MAX_PIDS];
static int n_tracked = 0;

/* Open perf event for a single PID */
static int open_perf_pid(pid_t pid, int freq) {
    struct perf_event_attr pe = {0};
    pe.type        = PERF_TYPE_HARDWARE;
    pe.config      = PERF_COUNT_HW_CACHE_MISSES;
    pe.size        = sizeof(pe);
    pe.freq        = 1;
    pe.sample_freq = freq;
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME;
    pe.disabled    = 1;          /* start disabled; enable after mmap */
    pe.exclude_kernel = 1;
    pe.exclude_hv     = 1;

    int fd = syscall(SYS_perf_event_open, &pe, pid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[DAEMON] perf_event_open pid=%d: %s\n", pid, strerror(errno));
        return -1;
    }
    return fd;
}

/* Read PIDs from cgroup.procs and open a perf fd for each */
static int open_perf_for_cgroup(const char *cgroup_procs_path, int freq) {
    FILE *f = fopen(cgroup_procs_path, "r");
    if (!f) { perror("fopen cgroup.procs"); return -1; }

    int page_size = sysconf(_SC_PAGESIZE);
    n_tracked = 0;
    pid_t pid;

    while (fscanf(f, "%d", &pid) == 1 && n_tracked < MAX_PIDS) {
        int fd = open_perf_pid(pid, freq);
        if (fd < 0) continue;

        void *buf = mmap(NULL, (MMAP_PAGES + 1) * page_size,
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (buf == MAP_FAILED) {
            fprintf(stderr, "[DAEMON] mmap pid=%d: %s\n", pid, strerror(errno));
            close(fd);
            continue;
        }

        /* Enable counter NOW — after mmap is ready to receive records */
        ioctl(fd, PERF_EVENT_IOC_RESET,  0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

        tracked[n_tracked].pid = pid;
        tracked[n_tracked].fd  = fd;
        tracked[n_tracked].buf = buf;
        n_tracked++;
        printf("[DAEMON] Tracking pid=%d\n", pid);
    }

    fclose(f);
    printf("[DAEMON] Tracking %d PIDs total\n", n_tracked);
    return n_tracked > 0 ? 0 : -1;
}

static void close_all() {
    int page_size = sysconf(_SC_PAGESIZE);
    for (int i = 0; i < n_tracked; i++) {
        if (tracked[i].buf) munmap(tracked[i].buf, (MMAP_PAGES + 1) * page_size);
        if (tracked[i].fd >= 0) close(tracked[i].fd);
    }
    n_tracked = 0;
}

/* Fetch trace context from app-side Unix socket */
static int fetch_trace_context(pid_t tid, char *trace_id, char *span_id) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/tmp/trace_bridge.sock");

    struct timeval tv = {0, 100000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock); return -1;
    }

    char req[64];
    snprintf(req, sizeof(req), "QUERY %d\n", tid);
    send(sock, req, strlen(req), 0);

    char resp[256] = {0};
    ssize_t n = recv(sock, resp, sizeof(resp)-1, 0);
    close(sock);

    if (n > 0 && sscanf(resp, "%63s %63s", trace_id, span_id) == 2) return 0;
    return -1;
}

/* OTLP export */
static int export_to_otel(const char *json) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, OTLP_ENDPOINT);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? 0 : -1;
}

/* Drain ring buffer for one tracked entry */
static void drain(tracked_t *t) {
    int page_size = sysconf(_SC_PAGESIZE);
    struct perf_event_mmap_page *hdr = t->buf;
    char *data = (char*)t->buf + page_size;

    __sync_synchronize();
    uint64_t head = hdr->data_head;
    uint64_t tail = hdr->data_tail;


    while (tail < head) {
        struct perf_event_header *rec =
            (void*)(data + (tail % (MMAP_PAGES * page_size)));

        if (rec->type == PERF_RECORD_SAMPLE) {
            struct { uint64_t ip; uint32_t pid; uint32_t tid; uint64_t time; } *s =
                (void*)(rec + 1);

            char trace_id[64] = "0000000000000000000000000000000000000000000000000000000000000000";
            char span_id[64]  = "0000000000000000";
            fetch_trace_context((pid_t)s->tid, trace_id, span_id);

            printf("[DAEMON] sample pid=%u tid=%u trace=%s\n",
                   s->pid, s->tid, trace_id);

            char json[2048];
            snprintf(json, sizeof(json),
                "{\"resourceSpans\":[{\"resource\":{\"attributes\":[{\"key\":\"service.name\","
                "\"value\":{\"stringValue\":\"perf-daemon\"}}]},\"scopeSpans\":[{\"spans\":["
                "{\"traceId\":\"%s\",\"spanId\":\"%s\",\"name\":\"pmu_sample\","
                "\"startTimeUnixNano\":%lu,\"endTimeUnixNano\":%lu,"
                "\"attributes\":[{\"key\":\"pmu.event\",\"value\":{\"stringValue\":\"cache-misses\"}},"
                "{\"key\":\"tid\",\"value\":{\"intValue\":%u}}]}]}]}]}",
                trace_id, span_id, s->time, s->time + 1000, s->tid);

            export_to_otel(json);
        } else if (rec->type == PERF_RECORD_LOST) {
            printf("[DAEMON] lost samples\n");
        }

        if (rec->size == 0) break;
        tail += rec->size;
    }

    __sync_synchronize();
    hdr->data_tail = head;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <cgroup.procs path> [freq_hz]\n", argv[0]);
        return 1;
    }

    signal(SIGINT,  handle_sigint);
    signal(SIGTERM, handle_sigint);
    curl_global_init(CURL_GLOBAL_ALL);

    const char *cgroup_procs = argv[1];
    int freq = (argc >= 3) ? atoi(argv[2]) : 100;

    printf("[DAEMON] Starting at %d Hz\n", freq);

    if (open_perf_for_cgroup(cgroup_procs, freq) < 0) {
        fprintf(stderr, "[DAEMON] Failed to open any perf events\n");
        return 1;
    }

    printf("[DAEMON] Running — polling ring buffers\n");

    while (running) {
        for (int i = 0; i < n_tracked; i++)
            drain(&tracked[i]);
        usleep(10000); /* 10ms poll */
    }

    close_all();
    curl_global_cleanup();
    printf("[DAEMON] Shutdown complete\n");
    return 0;
}
