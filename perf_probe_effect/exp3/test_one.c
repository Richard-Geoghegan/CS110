// #include <papiStdEventDefs.h>
#include <stdio.h>
#include <stdlib.h>
#include <papi.h>
#include <time.h>

#define SIZE 1000

void matrix_multiply(double **A, double **B, double **C) {
    for (int i = 0; i < SIZE; i++) {
        for (int j = 0; j < SIZE; j++) {
            for (int k = 0; k < SIZE; k++) {
                C[i][j] += A[i][k] * B[k][j];
            }
        }
    }
}

int main() {
    int retval;
    int EventSet = PAPI_NULL;
    int Events[2] = { PAPI_TOT_CYC, PAPI_TOT_INS};
    long long values[2];
    
    // 1. Initialize PAPI Library
    retval = PAPI_library_init(PAPI_VER_CURRENT);
    if (retval != PAPI_VER_CURRENT) {
        fprintf(stderr, "PAPI library init error!\n");
        exit(1);
    }

    // 2. Create the EventSet
    if (PAPI_create_eventset(&EventSet) != PAPI_OK) {
        fprintf(stderr, "Could not create EventSet\n");
        exit(1);
    }

    // 3. Add individual events (Total Instructions and Total Cycles)
    // if (PAPI_add_event(EventSet, PAPI_TOT_INS) != PAPI_OK) {
    //     fprintf(stderr, "Could not add PAPI_TOT_INS\n");
    // }
    // if (PAPI_add_event(EventSet, PAPI_TOT_CYC) != PAPI_OK) {
    //     fprintf(stderr, "Could not add PAPI_TOT_CYC\n");
    // }
    //
    retval = PAPI_add_events(EventSet, Events, 2);

    // --- Matrix Allocation ---
    double **A = malloc(SIZE * sizeof(double *));
    double **B = malloc(SIZE * sizeof(double *));
    double **C = malloc(SIZE * sizeof(double *));
    for (int i = 0; i < SIZE; i++) {
        A[i] = malloc(SIZE * sizeof(double));
        B[i] = malloc(SIZE * sizeof(double));
        C[i] = calloc(SIZE, sizeof(double)); // Initialize to zero
    }

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    // 4. Start counting the specific EventSet
    if (PAPI_start(EventSet) != PAPI_OK) {
        fprintf(stderr, "PAPI start error\n");
        exit(1);
    }

    matrix_multiply(A, B, C);

    // 5. Stop counting and read values
    if (PAPI_stop(EventSet, values) != PAPI_OK) {
        fprintf(stderr, "PAPI stop error\n");
        exit(1);
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double time_taken = (end_time.tv_sec - start_time.tv_sec) + 
                        (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    // Output results
    printf("Execution Time: %f seconds\n", time_taken);
    printf("Instructions: %lld\n", values[0]);
    printf("Cycles: %lld\n", values[1]);
    if (values[1] > 0) {
        printf("Calculated IPC: %f\n", (double)values[0] / values[1]);
    }

    // 6. Cleanup
    PAPI_cleanup_eventset(EventSet);
    PAPI_destroy_eventset(&EventSet);
    
    // Free memory...
    for(int i=0; i<SIZE; i++) { free(A[i]); free(B[i]); free(C[i]); }
    free(A); free(B); free(C);

    return 0;
}
