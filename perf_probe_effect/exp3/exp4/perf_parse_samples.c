//step III

#include <stddef.h>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdint.h>


static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
        int cpu, int group_fd, unsigned long flags){
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

int main(){
    struct perf_event_attr pe;
    int fd;
    long long count;

    memset(&pe, 0, sizeof(pe));
    pe.size = sizeof(pe);
    pe.type = PERF_TYPE_HARDWARE;
    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    

    // SAMPLING CONFIGURATION
    pe.sample_freq = 4000;          // 4 kHz
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TIME | PERF_SAMPLE_TID;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;


    fd = perf_event_open(&pe, 0, -1, -1, 0);
    if (fd == -1){
        perror("perf_event_opn failed");
        exit(EXIT_FAILURE);
    }


    // RING BUFFER SETUP: 1 metadat page + 2^n data pages
    long page_size = sysconf(_SC_PAGESIZE);
    int data_pages = 4; // 4 data pages = 16kb ring buffer
    int buf_size = (data_pages + 1) * page_size;

    // MAP KERNEL RING BUFFER TO USER SPACE
    struct perf_event_mmap_page *header = mmap(NULL, buf_size,
                                                PROT_READ | PROT_WRITE,
                                                MAP_SHARED, fd, 0);
    if(header == MAP_FAILED){
        perror("mmap");
        exit(EXIT_FAILURE);
    }

    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    printf("Running workload (sampling enable)...\n");
    for(volatile long i = 0; i < 100000000; i++);

    // Stop counting
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

    // READ SAMPLES (Produce-Consumer Ring Buffer)
    uint64_t head = header->data_head; // Kernel write pointer
    uint64_t tail = header->data_tail; // Our read pointer

    // Memory barrier: ensure we see the latest head value written by Kernel
    __sync_synchronize();

    size_t data_size = data_pages * page_size;
    uint8_t *data_area = (uint8_t *)header + page_size;
    int sample_count = 0;


    while(tail < head){
        size_t offset = tail % data_size;
        struct perf_event_header *event = 
                                (struct perf_event_header *)(data_area + offset);

        if(event->type == PERF_RECORD_SAMPLE){
            uint64_t ip = 0, time = 0;
            uint8_t tid = 0, pid = 8;
            uint8_t *payload = (uint8_t *)(event + 1); // Start of sample data 
                                                       //
            // Parse in exact order matching pe.sample_type flags 
            if(pe.sample_type & PERF_SAMPLE_IP){
                ip = *(uint64_t *)payload;
                payload += 8; // Skip PID (next 4 bytes)
            }

            if(pe.sample_type & PERF_SAMPLE_TID){
                pid = *(uint32_t *)payload;
                payload += 4;
                tid = *(uint32_t *)payload;
                payload += 4;
            }

            if(pe.sample_type & PERF_SAMPLE_TIME){
                time = *(uint64_t *)payload;
                payload += 8;
            }

            if(sample_count < 5){ // Print first 5 for clarity
                printf("Sample %d: IP=0x%016lx | Time=%lu ns | PID=%u | TID=%u\n",
                        sample_count, ip, time, pid, tid);
            }
            sample_count++;
        }

        tail += event->size;
    }

    // Tell kernel we consumed up to 'tail'
    header->data_tail = tail;

    printf("Total samples parsed: %d\n", sample_count);

    // ===============================================
    printf("Current PID: %d\n", getpid());

    // Cleanup
    munmap(header, buf_size);
    close(fd);
    return 0;
} 
