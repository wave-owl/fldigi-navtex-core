/*
 * fldigi-navtex-core against fldigi 4.2.13 itself (goldens from
 * tests/golden/make_fldigi_golden.py, the headless fldigi bench build with the NAVTEX dump
 * hooks of tests/golden/fldigi-bench-navtex.patch).
 *
 * TX, NAVTEX and SITOR-B, CQ, pangram (figures), multi-line, lower-case, ITA2 and a 1500 Hz
 * carrier: the port renders fldigi's samples (FNV-1a hash of the doubles fldigi hands to
 * ModulateXmtr and of the int16 file identical on the reference platform; at most 1 LSB
 * from the stored int16 vectors everywhere).
 * RX, the stored vectors clean and at -3 .. -12 dB (2500 Hz), 10 Hz mistuned, two messages
 * back to back, the framing cases (lost header, lost trailer, 600 s timeout), SITOR-B
 * received in NAVTEX mode and 20 s of pure noise: the printed bytes, the logged messages
 * and the logic levels handed to handle_bit_value equal fldigi's on the reference platform;
 * elsewhere (another libm) the clean decodes stay within 1 + 5 % edit distance of fldigi's
 * and the noisy ones within 20 % + 10 in total.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L

#include "test_util.h"

static const char *golden = "tests/golden";

static uint32_t lcg;
static double lcg_uniform(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return lcg / 4294967296.0;
}

#define NSNR 5
static const char *snr_names[NSNR] = {"clean", "m03", "m06", "m09", "m12"};
static const double snr_k[NSNR] = {0.0, 1.9952623149688795, 3.9810717055349722,
                                   7.943282347242816, 15.848931924611133};

/* Same as noisy() in make_fldigi_golden.py; this file is compiled without FP contraction,
 * so every step rounds like Python. */
static double *noisy(const int16_t *pcm, size_t n, int sr, int snr_step, uint32_t seed,
                     int tail_s, size_t *out_n)
{
    size_t lead = (size_t)sr, tail = (size_t)tail_s * (size_t)sr, total = lead + n + tail, i;
    double *x = (double *)calloc(total, sizeof(double));
    double ps = 0, s2, sd, gain, k = snr_k[snr_step];
    int j;
    for (i = 0; i < n; i++)
        x[lead + i] = (double)pcm[i];
    *out_n = total;
    if (snr_step == 0) {
        for (i = 0; i < total; i++)
            x[i] /= 32768.0;
        return x;
    }
    for (i = 0; i < n; i++)
        ps += (double)pcm[i] * (double)pcm[i];
    ps = ps / (double)n;
    s2 = ps * k * (sr / 2.0) / 2500.0;
    sd = sqrt(s2);
    gain = 3276.7 / sqrt(ps * (double)n / (double)total + s2);
    lcg = seed;
    for (i = 0; i < total; i++) {
        double z = 0.0, y, q;
        for (j = 0; j < 12; j++)
            z += lcg_uniform();
        z -= 6.0;
        y = (x[i] + sd * z) * gain;
        q = floor(y + 0.5);
        if (q > 32767)
            q = 32767;
        if (q < -32768)
            q = -32768;
        x[i] = q / 32768.0;  /* libsndfile sf_readf_double: int16 / 32768 */
    }
    return x;
}

static double *noise_only(int sr, int seconds, uint32_t seed, size_t *out_n)
{
    size_t total = (size_t)seconds * (size_t)sr, i;
    double *x = (double *)malloc(total * sizeof(double));
    int j;
    lcg = seed;
    for (i = 0; i < total; i++) {
        double z = 0.0, q;
        for (j = 0; j < 12; j++)
            z += lcg_uniform();
        z -= 6.0;
        q = floor(z * 3276.7 + 0.5);
        if (q > 32767)
            q = 32767;
        if (q < -32768)
            q = -32768;
        x[i] = q / 32768.0;
    }
    *out_n = total;
    return x;
}

#define NTEXTS 16
static char text_keys[NTEXTS][32];
static char text_vals[NTEXTS][256];
static int ntexts;

static const char *text_for(const char *key)
{
    int i;
    for (i = 0; i < ntexts; i++)
        if (strcmp(key, text_keys[i]) == 0)
            return text_vals[i];
    return "";
}

static void load_texts(void)
{
    char path[512], *buf, *line, *save = NULL;
    size_t len;
    snprintf(path, sizeof path, "%s/texts.txt", golden);
    buf = (char *)read_file(path, &len);
    if (!buf) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(2);
    }
    for (line = strtok_r(buf, "\n", &save); line && ntexts < NTEXTS;
         line = strtok_r(NULL, "\n", &save)) {
        char *sp = strchr(line, ' ');
        size_t k = 0;
        const char *p;
        if (!sp)
            continue;
        *sp = 0;
        snprintf(text_keys[ntexts], sizeof text_keys[ntexts], "%s", line);
        for (p = sp + 1; *p && k + 1 < sizeof text_vals[ntexts]; p++) {
            if (p[0] == '\\' && p[1] == 'n') {
                text_vals[ntexts][k++] = '\n';
                p++;
            } else {
                text_vals[ntexts][k++] = *p;
            }
        }
        text_vals[ntexts][k] = 0;
        ntexts++;
    }
    free(buf);
}

static int mode_of(const char *s)
{
    return strcmp(s, "SITORB") == 0 ? FNAVTEX_MODE_SITORB : FNAVTEX_MODE_NAVTEX;
}

static int16_t *load_s16(const char *stem, size_t *n)
{
    char path[512];
    size_t len;
    int16_t *p;
    snprintf(path, sizeof path, "%s/tx/%s.s16", golden, stem);
    p = (int16_t *)read_file(path, &len);
    *n = p ? len / 2 : 0;
    return p;
}

static int tx_ok, tx_total;

static void test_tx(void)
{
    char path[512], *buf, *line, *save = NULL;
    size_t len;
    snprintf(path, sizeof path, "%s/tx_hashes.txt", golden);
    buf = (char *)read_file(path, &len);
    CHECK(buf != NULL, "cannot read %s", path);
    if (!buf)
        return;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char stem[64], mode[16], key[32];
        int idx, carrier, ita2;
        size_t samples, n, i, sn;
        unsigned long long h16, hd;
        double *x;
        int16_t *pcm, *stored;
        int ok = 1;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %15s %31s %d %d %d %zu %llx %llx", stem, mode, key, &idx,
                   &carrier, &ita2, &samples, &h16, &hd) != 9)
            continue;
        tx_total++;
        x = render(mode_of(mode), carrier, ita2, text_for(key), &n);
        CHECK(n == samples, "tx %s: %zu samples, fldigi %zu", stem, n, samples);
        if (n != samples) {
            free(x);
            continue;
        }
        pcm = (int16_t *)malloc(n * sizeof(int16_t));
        quantize(x, n, pcm);
        if (REFERENCE_PLATFORM) {
            if (fnv_doubles(x, n) != hd) {
                CHECK(0, "tx %s: doubles differ from fldigi's", stem);
                ok = 0;
            }
            if (fnv_pcm(pcm, n) != h16) {
                CHECK(0, "tx %s: int16 hash differs from fldigi's", stem);
                ok = 0;
            }
        }
        stored = load_s16(stem, &sn);
        if (stored) {
            CHECK(sn == n, "tx %s: stored vector has %zu samples", stem, sn);
            for (i = 0; i < n && i < sn; i++) {
                if (abs((int)pcm[i] - (int)stored[i]) > 1) {
                    CHECK(0, "tx %s: sample %zu %d, fldigi %d", stem, i, pcm[i], stored[i]);
                    ok = 0;
                    break;
                }
            }
            free(stored);
        }
        tx_ok += ok;
        free(pcm);
        free(x);
    }
    free(buf);
}

static int rx_ok, rx_total;
static size_t noisy_dist, noisy_len;

static void check_rx(const char *name, const tbuf *text, const tbuf *msgs, uint64_t bh,
                     size_t bn, int clean)
{
    char path[512], line[256];
    size_t glen = 0, mlen = 0;
    char *gold, *gmsg;
    FILE *f;
    int ok = 1;
    snprintf(path, sizeof path, "%s/rx/%s.txt", golden, name);
    gold = (char *)read_file(path, &glen);
    snprintf(path, sizeof path, "%s/rx/%s.msg", golden, name);
    gmsg = (char *)read_file(path, &mlen);
    CHECK(gold && gmsg, "rx %s: golden missing", name);
    if (!gold || !gmsg) {
        free(gold);
        free(gmsg);
        return;
    }
    rx_total++;
    if (REFERENCE_PLATFORM) {
        unsigned long long gh = 0;
        size_t gn = 0;
        if (text->n != glen || memcmp(text->p ? text->p : "", gold, glen) != 0) {
            CHECK(0, "rx %s: printed text differs from fldigi's (%zu vs %zu bytes)", name,
                  text->n, glen);
            ok = 0;
        }
        if (msgs->n != mlen || memcmp(msgs->p ? msgs->p : "", gmsg, mlen) != 0) {
            CHECK(0, "rx %s: logged messages differ from fldigi's", name);
            ok = 0;
        }
        snprintf(path, sizeof path, "%s/rx_bits.txt", golden);
        f = fopen(path, "r");
        while (f && fgets(line, sizeof line, f)) {
            char nm[96];
            size_t n;
            unsigned long long h;
            if (sscanf(line, "%95s %zu %llx", nm, &n, &h) == 3 && strcmp(nm, name) == 0) {
                gh = h;
                gn = n;
            }
        }
        if (f)
            fclose(f);
        if (gh != bh || gn != bn) {
            CHECK(0, "rx %s: logic levels differ from fldigi's (%zu vs %zu)", name, bn, gn);
            ok = 0;
        }
    } else {
        size_t d = edit_distance(text->p ? text->p : "", text->n, gold, glen);
        if (clean) {
            if (d > 1 + glen / 20) {
                CHECK(0, "rx %s: edit distance %zu to fldigi's decode", name, d);
                ok = 0;
            }
        } else {
            noisy_dist += d;
            noisy_len += glen;
        }
    }
    rx_ok += ok;
    free(gold);
    free(gmsg);
}

static void test_rx(void)
{
    char path[512], *buf, *line, *save = NULL;
    size_t len;
    snprintf(path, sizeof path, "%s/rx_cases.txt", golden);
    buf = (char *)read_file(path, &len);
    CHECK(buf != NULL, "cannot read %s", path);
    if (!buf)
        return;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char stem[64], srcs[128], mode[16], snrs[128], *src, *sv = NULL;
        int idx, carrier, ita2, tail, s;
        int16_t *pcm = NULL;
        size_t n = 0;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %127s %15s %d %d %d %d %127s", stem, srcs, mode, &idx, &carrier,
                   &ita2, &tail, snrs) != 8)
            continue;
        for (src = strtok_r(srcs, "+", &sv); src; src = strtok_r(NULL, "+", &sv)) {
            size_t k;
            int16_t *p = load_s16(src, &k);
            CHECK(p != NULL, "rx %s: source %s missing", stem, src);
            if (!p)
                continue;
            pcm = (int16_t *)realloc(pcm, (n + k) * sizeof(int16_t));
            memcpy(pcm + n, p, k * sizeof(int16_t));
            n += k;
            free(p);
        }
        for (s = 0; s < NSNR; s++) {
            char name[96];
            size_t xn, bn;
            uint64_t bh;
            double *x;
            fnavtex_rx *rx;
            tbuf text = {NULL, 0, 0}, msgs = {NULL, 0, 0};
            if (!strstr(snrs, snr_names[s]))
                continue;
            x = noisy(pcm, n, FNAVTEX_RATE, s, (uint32_t)(1000 * idx + s), tail, &xn);
            rx = make_rx(mode_of(mode), carrier, ita2, 0);
            decode_all(rx, x, xn, 4096, &text, &msgs, &bh, &bn);
            snprintf(name, sizeof name, "%s_%s", stem, snr_names[s]);
            check_rx(name, &text, &msgs, bh, bn, s == 0);
            fnavtex_rx_destroy(rx);
            free(x);
            free(text.p);
            free(msgs.p);
        }
        free(pcm);
    }
    free(buf);
}

static void test_noise(void)
{
    static const struct { const char *name; int mode; uint32_t seed; } cases[2] = {
        {"navtex_noise", FNAVTEX_MODE_NAVTEX, 77}, {"sitorb_noise", FNAVTEX_MODE_SITORB, 78}};
    int c;
    for (c = 0; c < 2; c++) {
        size_t xn = 0, bn = 0, glen = 0;
        uint64_t bh;
        double *x = noise_only(FNAVTEX_RATE, 20, cases[c].seed, &xn);
        fnavtex_rx *rx = make_rx(cases[c].mode, 1000, 0, 0);
        tbuf text = {NULL, 0, 0}, msgs = {NULL, 0, 0};
        char path[512], *gold;
        decode_all(rx, x, xn, 4096, &text, &msgs, &bh, &bn);
        if (REFERENCE_PLATFORM) {
            check_rx(cases[c].name, &text, &msgs, bh, bn, 1);
        } else {
            snprintf(path, sizeof path, "%s/rx/%s.txt", golden, cases[c].name);
            gold = (char *)read_file(path, &glen);
            CHECK(gold && text.n <= glen + 20, "rx %s: %zu bytes from noise", cases[c].name,
                  text.n);
            free(gold);
        }
        fnavtex_rx_destroy(rx);
        free(x);
        free(text.p);
        free(msgs.p);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1)
        golden = argv[1];
    load_texts();
    test_tx();
    test_rx();
    test_noise();
    if (!REFERENCE_PLATFORM && noisy_len > 0)
        CHECK(noisy_dist <= noisy_len / 5 + 10, "noisy decodes: %zu edits over %zu bytes",
              noisy_dist, noisy_len);
    printf("fldigi-navtex-core vs fldigi 4.2.13: TX %d/%d, RX %d/%d identical%s\n", tx_ok,
           tx_total, rx_ok, rx_total, REFERENCE_PLATFORM ? "" : " (tolerance mode)");
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("fldigi-navtex-core fldigi goldens: all tests passed (%s)\n", fnavtex_version());
    return 0;
}
