/*
 * fldigi-navtex-core CPU and memory benchmark: receiver and transmitter as multiples of
 * real time on one core, bytes allocated per receiver.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L

#include "test_util.h"

#include <time.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void)
{
    static const char *text = "ZCZC TEST MESSAGE. THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG "
                              "0123456789. WIND SW 4 TO 5, SEA STATE 3 M, VISIBILITY GOOD.";
    int mode;
    for (mode = 0; mode < 2; mode++) {
        size_t n, i, reps = 8, total = 0;
        double *x = render(mode, 1000, 0, text, &n), t0, t1, t2, sec;
        fnavtex_rx *rx = make_rx(mode, 1000, 0, 0);
        static fnavtex_event ev[4096];
        uint32_t s = 1;
        for (i = 0; i < n; i++) {
            s = s * 1664525u + 1013904223u;
            x[i] = x[i] * 0.25 + 0.02 * ((double)s / 4294967296.0 - 0.5);
        }
        t0 = now();
        for (i = 0; i < reps; i++) {
            fnavtex_rx_process(rx, x, n, ev, 4096);
            total += n;
        }
        t1 = now();
        sec = (double)total / FNAVTEX_RATE;
        {
            size_t k;
            double *y = render(mode, 1000, 0, text, &k);
            for (i = 1; i < reps; i++) {
                double *z = render(mode, 1000, 0, text, &k);
                free(z);
            }
            free(y);
        }
        t2 = now();
        printf("%s: receiver %.0f x real time, transmitter %.0f x real time, %zu bytes per "
               "receiver\n", mode ? "SITOR-B" : "NAVTEX", sec / (t1 - t0),
               (double)(reps * n) / FNAVTEX_RATE / (t2 - t1), fnavtex_rx_footprint(rx));
        fnavtex_rx_destroy(rx);
        free(x);
    }
    return 0;
}
