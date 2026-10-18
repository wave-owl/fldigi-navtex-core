/*
 * fldigi-navtex-core test helpers (header-only).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NAVTEX_TEST_UTIL_H
#define NAVTEX_TEST_UTIL_H

#include "fldigi_navtex.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures __attribute__((unused));

#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                         \
            fprintf(stderr, __VA_ARGS__);                                                \
            fputc('\n', stderr);                                                         \
        }                                                                                \
    } while (0)

/* The platform the goldens were made on (fldigi 4.2.13 bench, macOS arm64, Apple clang -O2
 * with its default FP contraction): there the port must reproduce fldigi bit for bit. */
#if defined(__APPLE__) && defined(__aarch64__) && !defined(NAVTEX_NOT_REFERENCE)
#define REFERENCE_PLATFORM 1
#else
#define REFERENCE_PLATFORM 0
#endif

static inline void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf || (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

static inline uint64_t fnv1a64_bytes(const void *data, size_t n, uint64_t h)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

#define FNV_INIT 0xCBF29CE484222325ULL

/* little-endian int16 / double bytes as Python writes them */
static inline uint64_t fnv_pcm(const int16_t *pcm, size_t n)
{
    uint64_t h = FNV_INIT;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char b[2];
        uint16_t u = (uint16_t)pcm[i];
        b[0] = (unsigned char)(u & 0xFF);
        b[1] = (unsigned char)(u >> 8);
        h = fnv1a64_bytes(b, 2, h);
    }
    return h;
}

static inline uint64_t fnv_doubles(const double *x, size_t n)
{
    uint64_t h = FNV_INIT;
    size_t i;
    int j;
    for (i = 0; i < n; i++) {
        uint64_t u;
        unsigned char b[8];
        memcpy(&u, &x[i], 8);
        for (j = 0; j < 8; j++)
            b[j] = (unsigned char)(u >> (8 * j));
        h = fnv1a64_bytes(b, 8, h);
    }
    return h;
}

/* fldigi's sound path: modem::ModulateXmtr scales by 10^(txlevel/20), txlevel -12 dB, and
 * clips at +-0.95 (trx/modem.cxx:652-658); libsndfile writes PCM16 as lrint(x * 0x7FFF). */
static inline void quantize(const double *x, size_t n, int16_t *pcm)
{
    double mult = pow(10, -12.0 / 20.0);
    size_t i;
    for (i = 0; i < n; i++) {
        double v = x[i] * mult;
        if (v < -0.95)
            v = -0.95;
        if (v > 0.95)
            v = 0.95;
        pcm[i] = (int16_t)lrint(v * 32767.0);
    }
}

/* One whole message as fldigi's bench sends it (one message, first and last). Returns a
 * malloc'ed buffer of *n samples (peak 0.9, before the TX level). */
static inline double *render(int mode, double carrier, int ita2, const char *text, size_t *n)
{
    fnavtex_tx_config c;
    fnavtex_tx *tx;
    double *x;
    size_t total, k = 0, r;
    memset(&c, 0, sizeof c);
    c.mode = mode;
    c.carrier_hz = carrier;
    c.ita2 = ita2;
    c.first_number = -1;
    tx = fnavtex_tx_create(&c);
    *n = 0;
    if (!tx)
        return NULL;
    total = fnavtex_tx_load(tx, text, strlen(text), 1, 1);
    x = (double *)malloc((total + 1) * sizeof(double));
    while ((r = fnavtex_tx_render(tx, x + k, 4000)) > 0)
        k += r;
    fnavtex_tx_destroy(tx);
    *n = k;
    return x;
}

static inline fnavtex_rx *make_rx(int mode, double carrier, int ita2, int afc)
{
    fnavtex_rx_config c;
    memset(&c, 0, sizeof c);
    c.mode = mode;
    c.carrier_hz = carrier;
    c.ita2 = ita2;
    c.afc = afc;
    return fnavtex_rx_create(&c);
}

typedef struct {
    char *p;
    size_t n, cap;
} tbuf;

static inline void tbuf_add(tbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 256;
        b->p = (char *)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

/* Decode x in chunks of `chunk` samples, then flush. Printed bytes to *text, messages as
 * fldigi's bench dump lines ("<origin> <subject> <number> <size> <hex>\n") to *msgs (may
 * be NULL), and the FNV-1a hash and count of the "<sample> <level>\n" bit lines. */
static inline void decode_all(fnavtex_rx *rx, const double *x, size_t n, size_t chunk,
                              tbuf *text, tbuf *msgs, uint64_t *bits_hash, size_t *bits_n)
{
    static fnavtex_event ev[8192];
    static long long sm[4096];
    static int lv[4096];
    static char mt[16384];
    uint64_t h = FNV_INIT;
    size_t off = 0, nb_total = 0;
    int last = 0;
    while (!last) {
        size_t k = n - off < chunk ? n - off : chunk, ne, i, nb;
        fnavtex_message m;
        ne = fnavtex_rx_process(rx, x + off, k, ev, 8192);
        off += k;
        if (off >= n) {
            ne += fnavtex_rx_flush(rx, ev + ne, 8192 - ne);
            last = 1;
        }
        for (i = 0; i < ne; i++)
            tbuf_add(text, (const char *)&ev[i].ch, 1);
        while ((nb = fnavtex_rx_take_bits(rx, sm, lv, 4096)) > 0) {
            char line[64];
            for (i = 0; i < nb; i++) {
                int l = snprintf(line, sizeof line, "%lld %d\n", sm[i], lv[i]);
                h = fnv1a64_bytes(line, (size_t)l, h);
            }
            nb_total += nb;
        }
        while (fnavtex_rx_take_message(rx, &m, mt, sizeof mt)) {
            char line[64];
            int l;
            if (!msgs)
                continue;
            l = snprintf(line, sizeof line, "%d %d %d %zu ", (int)(unsigned char)m.origin,
                         (int)(unsigned char)m.subject, m.number, m.body_len);
            tbuf_add(msgs, line, (size_t)l);
            for (i = 0; i < m.text_len; i++) {
                l = snprintf(line, sizeof line, "%02x", (unsigned char)mt[i]);
                tbuf_add(msgs, line, (size_t)l);
            }
            tbuf_add(msgs, "\n", 1);
        }
    }
    if (bits_hash)
        *bits_hash = h;
    if (bits_n)
        *bits_n = nb_total;
}

static inline size_t edit_distance(const char *a, size_t na, const char *b, size_t nb)
{
    size_t *prev = (size_t *)malloc((nb + 1) * sizeof(size_t));
    size_t *cur = (size_t *)malloc((nb + 1) * sizeof(size_t));
    size_t i, j, d;
    for (j = 0; j <= nb; j++)
        prev[j] = j;
    for (i = 1; i <= na; i++) {
        cur[0] = i;
        for (j = 1; j <= nb; j++) {
            size_t sub = prev[j - 1] + (a[i - 1] != b[j - 1]);
            size_t del = prev[j] + 1, ins = cur[j - 1] + 1;
            cur[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
        }
        memcpy(prev, cur, (nb + 1) * sizeof(size_t));
    }
    d = prev[nb];
    free(prev);
    free(cur);
    return d;
}

static inline int contains(const char *hay, size_t n, const char *needle)
{
    size_t k = strlen(needle), i;
    if (k == 0)
        return 1;
    for (i = 0; i + k <= n; i++)
        if (memcmp(hay + i, needle, k) == 0)
            return 1;
    return 0;
}

#endif
