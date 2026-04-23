// benchmark.c
// Compile: gcc -O2 -o benchmark benchmark.c -lm

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#define WORKLOAD_ITERATIONS 100000000

void cpu_intensive_work() {
    volatile double result = 0.0;
    for (long i = 0; i < WORKLOAD_ITERATIONS; i++) {
        result += sin(i * 0.001) * cos(i * 0.002);
        result += sqrt(i + 1.0);
    }
}

int main(int argc, char *argv[]) {
    struct timespec start, end;
    double elapsed;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        return 1;
    }

    cpu_intensive_work();

    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        perror("clock_gettime");
        return 1;
    }

    elapsed = (end.tv_sec - start.tv_sec) + 
              (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("Execution time: %.6f seconds\n", elapsed);
    return 0;
}
