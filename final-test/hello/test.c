#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L  /* Guarantees CLOCK_MONOTONIC & syscall visibility */
#include <unistd.h>
#include <time.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>

/* ================= CONFIGURATION ================= */
#define MMAP_PAGES         8      /* Pages for perf ring buffer */
#define BASELINE_FREQ      100    /* Default sampling rate (Hz) */
#define TRIGGERED_FREQ     4000   /* High-frequency sampling rate (Hz) */
#define CTL_SOCKET_PATH    "/tmp/perf_daemon_ctl"
#define TRIGGER_WINDOW_SEC 15    /* Auto-revert window for high-freq mode */

/* ================= GLOBAL STATE ================= */
static time_t last_trigger_time = 0;
static bool is_in_high_freq_mode = false;
static volatile sig_atomic_t running = 1;
static int perf_fd = -1;
static void *mmap_buf = NULL;
static int ctl_fd = -1;
static atomic_int current_freq = BASELINE_FREQ;
static const char *TARGET_CGROUP = NULL;
static uint64_t event_counter = 0; /* Monotonic ID counter */

/* ================= SIGNAL HANDLER ================= */
static void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

/* ================= PERF EVENT SETUP ================= */
/**
 * Opens a perf_event FD scoped to a cgroup v2 path.
 * Added PERF_SAMPLE_PERIOD to capture the actual event count per sample.
 */
static int open_perf_event(const char *cgroup_path, int freq) {
    printf("%s\n", cgroup_path);
    int cg_fd = open(cgroup_path, O_RDONLY);
    if (cg_fd < 0) { perror("cgroup open"); return -1; }

    struct perf_event_attr pe = {0};
    pe.type = PERF_TYPE_HARDWARE;
    pe.config = PERF_COUNT_HW_CACHE_MISSES;
    pe.size = sizeof(pe);
    pe.sample_freq = freq;
    /* IP | TIME | TID | ID | PERIOD layout */
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TIME | PERF_SAMPLE_TID | PERF_SAMPLE_ID | PERF_SAMPLE_PERIOD;
    pe.disabled = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.use_clockid = 1;
    pe.clockid = CLOCK_MONOTONIC;
    pe.cgroup = cg_fd;
    pe.wakeup_events = 1; /* Wake daemon immediately per sample for real-time console output */

    int fd = syscall(SYS_perf_event_open, &pe, -1, 0, -1, PERF_FLAG_FD_CLOEXEC);
    close(cg_fd);
    return fd;
}

/**
 * Dynamically toggles sampling frequency across the perf event.
 */
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
    } else {
        is_in_high_freq_mode = false;
    }
    return 0;
}

/* ================= MAIN ENTRY POINT ================= */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: sudo %s <cgroup_procs_path>\n", argv[0]);
        return 1;
    }
    TARGET_CGROUP = argv[1];

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
     setvbuf(stdout, NULL, _IONBF, 0); /* Real-time console output */

    perf_fd = open_perf_event(TARGET_CGROUP, BASELINE_FREQ);
    if (perf_fd < 0) return 1;

    int page_size = sysconf(_SC_PAGESIZE);
    mmap_buf = mmap(NULL, (MMAP_PAGES + 1) * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, perf_fd, 0);
    if (mmap_buf == MAP_FAILED) { perror("mmap"); return 1; }
    struct perf_event_mmap_page *header = mmap_buf;

    /* Setup control socket */
    ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ctl_addr = {.sun_family = AF_UNIX, .sun_path = CTL_SOCKET_PATH};
    unlink(CTL_SOCKET_PATH);
    bind(ctl_fd, (struct sockaddr*)&ctl_addr, sizeof(ctl_addr));
    listen(ctl_fd, 5);
    printf("[DAEMON] 🟢 Monitoring cgroup | Baseline: %d Hz | Control: %s\n", BASELINE_FREQ, CTL_SOCKET_PATH);

    char event_id[32];
    struct timespec ts;

    /* ================= EVENT LOOP ================= */
    while (running) {
        /* 1. Poll control socket (100ms timeout) */
        fd_set rfds; FD_ZERO(&rfds); FD_SET(ctl_fd, &rfds);
        struct timeval tv = {0, 100000};
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

        /* 2. Auto-revert check */
        if (is_in_high_freq_mode) {
            time_t elapsed = time(NULL) - last_trigger_time;
            if (elapsed >= TRIGGER_WINDOW_SEC) {
                printf("[DAEMON] Trigger window expired (%lds). Auto-reverting...\n", elapsed);
                toggle_sampling_freq(BASELINE_FREQ);
            }
        }

        /* 3. Read & parse ring buffer */
        __sync_synchronize();
        uint64_t head = header->data_head;
        uint64_t tail = header->data_tail;
        char *data = (char*)mmap_buf + page_size;

        while (tail < head) {
            struct perf_event_header *hdr = (struct perf_event_header*)(data + (tail % (MMAP_PAGES * page_size)));
            if (hdr->type == PERF_RECORD_SAMPLE) {
                /* Parse sample layout: IP | TIME | TID(pid,tid) | ID | PERIOD */
                uint8_t *ptr = (uint8_t*)(hdr + 1);
                ptr += sizeof(uint64_t);                      /* Skip IP */
                uint64_t time_ns = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
                uint32_t pid = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
                uint32_t tid = *(uint32_t*)ptr; ptr += sizeof(uint32_t);
                uint64_t sample_id = *(uint64_t*)ptr; ptr += sizeof(uint64_t);
                uint64_t period = *(uint64_t*)ptr; ptr += sizeof(uint64_t); /* Actual event count for this sample */

                (void)time_ns; (void)sample_id;

                /* Generate unique ID */
                event_counter++;
                snprintf(event_id, sizeof(event_id), "evt_%016lx", event_counter);

                /* Capture wall-clock time */
                clock_gettime(CLOCK_MONOTONIC, &ts);
                uint64_t wall_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

                /* Print NDJSON with actual sample count */
                printf("{\"id\":\"%s\",\"tid\":%u,\"pid\":%u,\"timestamp_ns\":%lu,\"wall_clock_ns\":%lu,\"event\":\"cache_miss\",\"count\":%lu}\n",
                       event_id, tid, pid, (unsigned long)time_ns, (unsigned long)wall_ns, (unsigned long)period);
            }
            tail += hdr->size;
        }

        /* Notify kernel of consumed data */
        __sync_synchronize();
        header->data_tail = tail; /* Fixed: must update to 'tail', not 'head' */
    }

    /* ================= CLEANUP ================= */
    printf("[DAEMON] Shutting down...\n");
    if (mmap_buf) munmap(mmap_buf, (MMAP_PAGES + 1) * page_size);
    if (perf_fd >= 0) {
        ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0);
        close(perf_fd);
    }
    close(ctl_fd);
    unlink(CTL_SOCKET_PATH);
    return 0;
}
