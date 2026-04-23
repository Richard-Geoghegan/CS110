#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>


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
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;


    fd = perf_event_open(&pe, 0, -1, -1, 0);
    if (fd == -1){
        perror("perf_event_opn failed");
        exit(EXIT_FAILURE);
    }

    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

    printf("Running workload...\n");
    for(volatile long i = 0; i < 100000000; i++);

    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    if(read(fd, &count, sizeof(count)) != sizeof(count)){
        perror("read failed");
        close(fd);
        exit(EXIT_FAILURE);
    }

    printf("Retired Instruction: %lld\n", count);
    close(fd);
    return 0;
}
