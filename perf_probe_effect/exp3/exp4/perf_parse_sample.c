#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>

static volatile sig_atomic_t running = 1;
void handle_sigint(int sig) { running = 0; }

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

// Convert kernel monotonic ns -> Unix epoch ns for OpenTelemetry
static int64_t get_time_offset_ns(void) {
    struct timespec mono, real;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    return (real.tv_sec - mono.tv_sec) * 1000000000LL + (real.tv_nsec - mono.tv_nsec);
}

int main() {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));
    pe.size = sizeof(pe);
    pe.type = PERF_TYPE_HARDWARE;
    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    pe.sample_freq = 4000;
    pe.freq = 1;
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TIME | PERF_SAMPLE_TID;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;

    int fd = perf_event_open(&pe, 0, -1, -1, 0);
    if (fd == -1) { perror("perf_event_open"); exit(EXIT_FAILURE); }

    long page_size = sysconf(_SC_PAGESIZE);
    int data_pages = 4;
    size_t buf_size = (data_pages + 1) * page_size;

    struct perf_event_mmap_page *header = mmap(NULL, buf_size,
                                               PROT_READ | PROT_WRITE,
                                               MAP_SHARED, fd, 0);
    if (header == MAP_FAILED) { perror("mmap"); exit(EXIT_FAILURE); }

    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    printf("⏳ Running workload (PMU sampling enabled)...\n");
    for (volatile long i = 0; i < 100000000; i++);

    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

    // DRAIN RING BUFFER (Synchronous for short workloads)
    int64_t time_offset = get_time_offset_ns();
    uint64_t head = header->data_head;
    uint64_t tail = header->data_tail;
    __sync_synchronize(); // Memory barrier: sync with kernel writes

    size_t data_size = data_pages * page_size;
    uint8_t *data_area = (uint8_t *)header + page_size;
    int exported = 0;

    while (tail < head) {
        size_t offset = tail % data_size;
        struct perf_event_header *hdr = (struct perf_event_header *)(data_area + offset);

        if (hdr->type == PERF_RECORD_SAMPLE) {
            uint64_t ip = 0, time = 0;
            uint32_t pid = 0, tid = 0;
            uint8_t *payload = (uint8_t *)(hdr + 1);

            if (pe.sample_type & PERF_SAMPLE_IP)   { ip = *(uint64_t *)payload; payload += 8; }
            if (pe.sample_type & PERF_SAMPLE_TID)  { pid = *(uint32_t *)payload; payload += 4; tid = *(uint32_t *)payload; payload += 4; }
            if (pe.sample_type & PERF_SAMPLE_TIME) { time = *(uint64_t *)payload; payload += 8; }

            int64_t unix_ns = time + time_offset;

            // OpenTelemetry-compatible JSON span (one per line)
            printf("{\"resourceSpans\":[{\"scopeSpans\":[{\"spans\":[{\"traceId\":\"%016lx%016lx\",\"spanId\":\"%016lx\",\"startTimeUnixNano\":\"%ld\",\"endTimeUnixNano\":\"%ld\",\"attributes\":{\"ip\":\"0x%lx\",\"pid\":%u,\"tid\":%u,\"event\":\"cpu-cycles\"}}]}]}]}\n",
                   (unsigned long)tid, (unsigned long)pid, (unsigned long)ip,
                   unix_ns, unix_ns + 1, (unsigned long)ip, pid, tid);
            exported++;
        }
        tail += hdr->size;
    }

    header->data_tail = tail; // Acknowledge consumed samples
    printf("\n✅ Exported %d spans to stdout. Pipe to collector: ./perf_otel_export | jq . > samples.json\n", exported);

    munmap(header, buf_size);
    close(fd);
    return 0;
}