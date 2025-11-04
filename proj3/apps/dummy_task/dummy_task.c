#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

/* Time helpers (must use gettimeofday per spec) */
static inline long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + (long long)tv.tv_usec;
}

/* Global calibration knob used by dummy_load() */
static int dummy_load_calib = 1;

/* Busy-wait loop. The outer-loop bound is the calibrated value. */
static void dummy_load(int execution_time_ms) {
    volatile int i, j;
    for (j = 0; j < dummy_load_calib * execution_time_ms; j++) {
        for (i = 0; i < 1000; i++) {
            __asm__ volatile("nop");
        }
    }
}

/* Measure how long dummy_load(ms) actually takes, in microseconds */
static long long measure_us(int ms) {
    long long t0 = now_us();
    dummy_load(ms);
    long long t1 = now_us();
    return t1 - t0;
}

/*
 * Auto-calibrate dummy_load_calib so that dummy_load(ref_ms)
 * runs within ±tolerance_us of ref_ms.
 */
static void calibrate(int ref_ms, int tolerance_us) {
    /* Start with a small guess and ensure we get a measurable time */
    dummy_load_calib = 1;
    long long us = measure_us(ref_ms);

    /* If measured time is ~0, grow quickly until we get something usable */
    while (us < 100) {                 /* <0.1 ms => too small to scale reliably */
        if (dummy_load_calib > 1000000) break;  /* safety guard */
        dummy_load_calib *= 10;
        us = measure_us(ref_ms);
    }

    /* Iteratively scale to target using proportional control */
    const int max_iters = 20;
    const double target_us = ref_ms * 1000.0;

    for (int k = 0; k < max_iters; k++) {
        long long err = (long long)(target_us - us);
        if (llabs(err) <= tolerance_us) {
            return;  /* good enough */
        }

        if (us <= 0) {
            /* extremely small => boost a lot */
            dummy_load_calib = (dummy_load_calib < 10) ? 10 : dummy_load_calib * 2;
        } else {
            /* proportional scaling: new = old * (target/actual) */
            double scale = target_us / (double)us;
            long long next = (long long)(dummy_load_calib * scale + 0.5);
            if (next < 1) next = 1;
            /* limit how wildly we jump to avoid overshoot ping-pong */
            if (next > (long long)dummy_load_calib * 10) next = (long long)dummy_load_calib * 10;
            dummy_load_calib = (int)next;
        }

        us = measure_us(ref_ms);
    }
    /* Fall-through: we did our best; the main program will still run. */
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <requested_ms>\n", argv[0]);
        return 1;
    }
    int requested_ms = atoi(argv[1]);
    if (requested_ms <= 0) {
        fprintf(stderr, "Error: requested_ms must be > 0\n");
        return 1;
    }

    /* 1) Calibrate once per program start (portable across machines) */
    const int ref_ms = 10;           /* reference duration to calibrate against */
    const int tol_us = 500;          /* ±0.5 ms tolerance required by spec */
    calibrate(ref_ms, tol_us);

    /* Optional: show calibration info (can keep for debugging) */
    // long long ref_us = measure_us(ref_ms);
    // printf("Calibration: dummy_load_calib=%d, ref %d ms -> %.3f ms\n",
    //        dummy_load_calib, ref_ms, ref_us / 1000.0);

    /* 2) Run the requested duration and report actual time */
    long long t0 = now_us();
    dummy_load(requested_ms);
    long long t1 = now_us();

    double actual_ms = (t1 - t0) / 1000.0;
    printf("Requested execution time: %d ms\n", requested_ms);
    printf("Actual execution time   : %.3f ms\n", actual_ms);
    return 0;
}
