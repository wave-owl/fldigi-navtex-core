/*
 * fldigi-navtex-core unit tests: CCIR 476 code, encoder, FEC cascade, framing, receiver and
 * transmitter properties (chunk invariance, loopback, noise, AFC, 64-bit sample clock,
 * occupied bandwidth) and the API contract.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L

#include "test_util.h"
#include "../src/navtex_internal.h"

#include <limits.h>

/* ---------------------------------------------------------------------------------- */

static uint32_t rng_state = 12345;
static double gauss(void)
{
    double z = 0;
    int j;
    for (j = 0; j < 12; j++) {
        rng_state = rng_state * 1664525u + 1013904223u;
        z += rng_state / 4294967296.0;
    }
    return z - 6.0;
}

/* 1 s silence + tx + 3 s silence at fldigi's TX level, plus white noise of rms `noise`. */
static double *with_noise(const double *tx, size_t n, double noise, size_t *out_n)
{
    size_t lead = FNAVTEX_RATE, tail = 3 * FNAVTEX_RATE, total = lead + n + tail, i;
    double *x = (double *)calloc(total, sizeof(double));
    double mult = pow(10, -12.0 / 20.0);
    for (i = 0; i < n; i++)
        x[lead + i] = tx[i] * mult;
    for (i = 0; i < total; i++)
        x[i] += noise * gauss();
    *out_n = total;
    return x;
}

static void decode_text(int mode, double carrier, int ita2, int afc, const double *x, size_t n,
                        size_t chunk, tbuf *text, tbuf *msgs)
{
    fnavtex_rx *rx = make_rx(mode, carrier, ita2, afc);
    decode_all(rx, x, n, chunk, text, msgs, NULL, NULL);
    fnavtex_rx_destroy(rx);
}

/* ---------------------------------------------------------------------------------- */

static void test_version(void)
{
    const char *v = fnavtex_version();
    CHECK(strstr(v, "fldigi-navtex-core " FNAVTEX_VERSION_STRING) == v, "version %s", v);
    CHECK(strstr(v, "contract=navtex-c1") != NULL, "contract tag");
    CHECK(strstr(v, "abi=1") != NULL, "abi tag");
    CHECK(strstr(v, "tx=1") != NULL, "tx tag");
    CHECK(strstr(v, "dsp=" FDSP_VERSION_STRING) != NULL, "dsp tag");
    CHECK(fnavtex_abi() == FNAVTEX_ABI, "abi");
}

static void test_create_refusals(void)
{
    fnavtex_rx_config rc;
    fnavtex_tx_config tc;
    fnavtex_rx *rx;
    memset(&rc, 0, sizeof rc);
    rc.mode = 2;
    CHECK(fnavtex_rx_create(&rc) == NULL, "bad mode");
    rc.mode = FNAVTEX_MODE_NAVTEX;
    rc.carrier_hz = 100;
    CHECK(fnavtex_rx_create(&rc) == NULL, "carrier too low");
    rc.carrier_hz = 3500;
    CHECK(fnavtex_rx_create(&rc) == NULL, "carrier too high");
    rc.carrier_hz = 0;
    rc.min_msg_len = -1;
    CHECK(fnavtex_rx_create(&rc) == NULL, "negative min_msg_len");
    rc.min_msg_len = 0;
    rx = fnavtex_rx_create(&rc);
    CHECK(rx != NULL && fnavtex_rx_carrier_hz(rx) == 1000.0, "default carrier");
    CHECK(fnavtex_rx_footprint(rx) > 100000, "footprint");
    fnavtex_rx_destroy(rx);
    CHECK(fnavtex_rx_create(NULL) == NULL, "NULL config");
    memset(&tc, 0, sizeof tc);
    tc.mode = 5;
    CHECK(fnavtex_tx_create(&tc) == NULL, "tx bad mode");
    tc.mode = FNAVTEX_MODE_SITORB;
    tc.first_number = 100;
    CHECK(fnavtex_tx_create(&tc) == NULL, "tx bad number");
    tc.first_number = -2;
    CHECK(fnavtex_tx_create(&tc) == NULL, "tx bad number -2");
    /* NULL-safe API */
    fnavtex_rx_destroy(NULL);
    fnavtex_tx_destroy(NULL);
    CHECK(fnavtex_rx_process(NULL, NULL, 0, NULL, 0) == 0, "NULL process");
    CHECK(fnavtex_rx_take_message(NULL, NULL, NULL, 0) == 0, "NULL take");
    CHECK(fnavtex_tx_load(NULL, "x", 1, 1, 1) == 0, "NULL load");
}

static void test_ccir476(void)
{
    int code, valid = 0, ltrs = 0, figs_us = 0, figs_ita2 = 0;
    unsigned char lt[128], fg[128];
    for (code = 0; code < 128; code++) {
        if (!fnavtex_check_bits(code))
            continue;
        valid++;
        if (fnavtex_code_to_ltrs[code] != '_')
            ltrs++;
        if (fnavtex_ustty_code_to_figs[code] != '_')
            figs_us++;
        if (fnavtex_ita2_code_to_figs[code] != '_')
            figs_ita2++;
    }
    /* 35 codes with four of seven bits set (CCIR 476): 29 letters/space/CR/LF and six
     * service codes */
    CHECK(valid == 35, "%d valid codes", valid);
    CHECK(ltrs == 29, "%d letter codes", ltrs);
    CHECK(figs_us == 29 && figs_ita2 == 29, "figure codes %d %d", figs_us, figs_ita2);
    CHECK(fnavtex_check_bits(NAVTEX_CODE_ALPHA) && fnavtex_check_bits(NAVTEX_CODE_REP) &&
          fnavtex_check_bits(NAVTEX_CODE_BETA) && fnavtex_check_bits(NAVTEX_CODE_CHAR32) &&
          fnavtex_check_bits(NAVTEX_CODE_LTRS) && fnavtex_check_bits(NAVTEX_CODE_FIGS),
          "service codes are valid codes");
    CHECK(fnavtex_code_to_char(0x47, 0, 0) == 'A', "A");
    CHECK(fnavtex_code_to_char(0x47, 1, 0) == '-', "figs of A");
    CHECK(fnavtex_code_to_char(0x4b, 1, 0) == 7, "US-TTY bell");
    CHECK(fnavtex_code_to_char(0x4b, 1, 1) == '\'', "ITA2 apostrophe");
    CHECK(fnavtex_code_to_char(0x3c, 1, 1) == '=', "ITA2 =");
    CHECK(fnavtex_code_to_char(0x0f, 0, 0) == -0x0f, "alpha has no character");
    /* encoder tables invert the decoder tables */
    fnavtex_encoder_tables(0, lt, fg);
    for (code = 'A'; code <= 'Z'; code++)
        CHECK(fnavtex_code_to_char(lt[code], 0, 0) == code, "letter %c", code);
    for (code = '0'; code <= '9'; code++)
        CHECK(fnavtex_code_to_char(fg[code], 1, 0) == code, "figure %c", code);
}

static void test_encoder(void)
{
    unsigned char lt[128], fg[128], out[64];
    size_t n;
    static const unsigned char want[] = {0x47, NAVTEX_CODE_FIGS, 0x2e, 0x5c, 0x27,
                                         NAVTEX_CODE_LTRS, 0x47};
    static const char *naut = "NAUTICAL";
    unsigned char codes[16], fec[64];
    size_t k, i;
    fnavtex_encoder_tables(0, lt, fg);
    /* "a1 2a": lower case is taken upper case, the space keeps the figures shift (it has a
     * code in both tables), unknown bytes and bytes above 127 are skipped */
    n = fnavtex_encode(lt, fg, "a1 2\x80{a", 7, out);
    CHECK(n == sizeof want && memcmp(out, want, n) == 0, "encode shifts (%zu codes)", n);
    /* create_fec: "NAUTICAL" -> rep alpha rep alpha N alpha A alpha U N T A I U C T A I L C
     * char32 A char32 L (navtex.cxx:1725-1727) */
    k = fnavtex_encode(lt, fg, naut, 8, codes);
    CHECK(k == 8, "NAUTICAL codes");
    n = fnavtex_create_fec(codes, k, fec);
    CHECK(n == 2 * 8 + 8, "fec length %zu", n);
    CHECK(fec[0] == NAVTEX_CODE_REP && fec[1] == NAVTEX_CODE_ALPHA &&
          fec[2] == NAVTEX_CODE_REP && fec[3] == NAVTEX_CODE_ALPHA, "fec preamble");
    CHECK(fec[4] == codes[0] && fec[5] == NAVTEX_CODE_ALPHA && fec[6] == codes[1] &&
          fec[7] == NAVTEX_CODE_ALPHA, "fec first characters");
    for (i = 2; i < 8; i++)
        CHECK(fec[4 + 2 * i] == codes[i] && fec[5 + 2 * i] == codes[i - 2], "fec pair %zu", i);
    CHECK(fec[20] == NAVTEX_CODE_CHAR32 && fec[21] == codes[6] &&
          fec[22] == NAVTEX_CODE_CHAR32 && fec[23] == codes[7], "fec tail");
}

/* ---------------------------------------------------------------------------------- */
/* FEC cascade (AC-890.2)                                                             */
/* ---------------------------------------------------------------------------------- */

static void set_char(int *pos, int code, int strength)
{
    int i;
    for (i = 0; i < 7; i++)
        pos[i] = ((code >> i) & 1) ? strength : -strength;
}

typedef struct {
    fnavtex_rx *rx;
    fnavtex_event ev[16];
} fec_bed;

static int fec_run(fec_bed *b, int ret_expect, int ch_expect, int fec_flag, const char *what)
{
    int ret;
    b->rx->ev = b->ev;
    b->rx->ev_max = 16;
    b->rx->ev_n = 0;
    ret = fnavtex_i_process_bytes(b->rx, 40);
    CHECK(ret == ret_expect, "%s: returned %d, want %d", what, ret, ret_expect);
    if (ch_expect >= 0) {
        CHECK(b->rx->ev_n == 1 && b->ev[0].ch == ch_expect, "%s: decoded %d chars, %c", what,
              (int)b->rx->ev_n, b->rx->ev_n ? b->ev[0].ch : '?');
        CHECK(b->rx->ev_n == 1 && (b->ev[0].flags & FNAVTEX_EV_FEC) == fec_flag,
              "%s: FEC flag", what);
    } else {
        CHECK(b->rx->ev_n == 0, "%s: nothing decoded", what);
    }
    b->rx->ev = NULL;
    return ret;
}

static void test_fec_cascade(void)
{
    fec_bed b;
    int *alpha, *rep;
    const int A = 0x47, E = 0x56;
    b.rx = make_rx(FNAVTEX_MODE_SITORB, 1000, 0, 0);
    alpha = &b.rx->bit_values[40];
    rep = &b.rx->bit_values[5];

    /* the alpha copy is valid */
    set_char(alpha, A, 1000);
    set_char(rep, E, 1000);
    fec_run(&b, 1, 'A', 0, "alpha valid");

    /* the alpha copy has five ones (invalid), the rep copy is valid */
    set_char(alpha, A | 0x08, 1000);
    set_char(rep, E, 1000);
    fec_run(&b, 0, 'E', FNAVTEX_EV_FEC, "rep replacement");

    /* a rep copy that is code_rep: alpha is probably code_alpha, nothing decoded */
    set_char(alpha, 0x7f, 1000);
    set_char(rep, NAVTEX_CODE_REP, 1000);
    fec_run(&b, 0, -1, 0, "rep is code_rep");

    /* both invalid, the sum of the two is valid: A = 1000111b; alpha misses bit 6 weakly
     * (-100 instead of +1000), rep has a weak extra bit 5 (+200), the sum gives A */
    set_char(alpha, A, 1000);
    alpha[6] = -100;
    set_char(rep, A, 1000);
    rep[3] = -400;
    rep[5] = 200;
    fec_run(&b, -1, 'A', FNAVTEX_EV_FEC, "average");

    /* both invalid with three ones, the average too; flipping the weakest zero of the alpha
     * copy gives a valid code */
    set_char(alpha, A, 1000);
    alpha[2] = -10;             /* A without bit 2: three ones, weakest zero at bit 2 */
    set_char(rep, 0x03, 1000);  /* two ones */
    fec_run(&b, -1, 'A', FNAVTEX_EV_FEC, "alpha bit flip");
    CHECK(alpha[2] == 10, "the flip stays in the bit stream as in fldigi");

    /* nothing works: hard failure, counted */
    set_char(alpha, 0x01, 1000);
    set_char(rep, 0x02, 1000);
    b.rx->msg_failed = 0;
    fec_run(&b, -2, -1, 0, "uncorrectable");
    CHECK(b.rx->msg_failed == 1, "failed character counted");

    /* flip_smallest_bit corner: count_one starts at 1 (fldigi), so four real ones and three
     * zeros flip the smallest one */
    {
        int pos[7] = {5, 6, 7, 8, -1, -2, -3};
        fnavtex_i_flip_smallest_bit(pos);
        CHECK(pos[0] == -5, "flip with count_one == 5");
    }
    fnavtex_rx_destroy(b.rx);
}

static void test_find_alpha(void)
{
    /* bits of a B-mode stream: find_alpha_characters must find the alpha phase */
    fnavtex_rx *rx = make_rx(FNAVTEX_MODE_SITORB, 1000, 0, 0);
    unsigned char lt[128], fg[128], codes[64], fec[160];
    size_t k, n, i;
    int bit, off, start = 3;
    fnavtex_encoder_tables(0, lt, fg);
    k = fnavtex_encode(lt, fg, "THE QUICK BROWN FOX", 19, codes);
    n = fnavtex_create_fec(codes, k, fec);
    for (i = 0; i < NAVTEX_BITS; i++) {
        size_t b = i + 7 * 4 + (size_t)start;   /* stream position */
        bit = (fec[(b / 7) % n] >> (b % 7)) & 1;
        rx->bit_values[i] = bit ? 1000 : -1000;
    }
    off = fnavtex_i_find_alpha_characters(rx);
    CHECK(off >= 35 && off < 49, "alpha phase found at %d", off);
    /* noise: no phase */
    for (i = 0; i < NAVTEX_BITS; i++)
        rx->bit_values[i] = (gauss() > 0) ? 1 : -1;
    CHECK(fnavtex_i_find_alpha_characters(rx) == -1, "no phase in noise");
    fnavtex_rx_destroy(rx);
}

/* ---------------------------------------------------------------------------------- */
/* framing (AC-890.3)                                                                 */
/* ---------------------------------------------------------------------------------- */

static void feed_text(fnavtex_rx *rx, const char *s)
{
    static fnavtex_event ev[1024];
    unsigned char lt[128], fg[128], codes[512];
    size_t k, i;
    rx->ev = ev;
    rx->ev_max = 1024;
    rx->ev_n = 0;
    fnavtex_encoder_tables(rx->ita2, lt, fg);
    k = fnavtex_encode(lt, fg, s, strlen(s), codes);
    /* the encoder starts in letters; so does the receiver for every text fed here */
    fnavtex_i_process_char(rx, NAVTEX_CODE_LTRS);
    for (i = 0; i < k; i++)
        fnavtex_i_process_char(rx, codes[i]);
    rx->ev = NULL;
}

static int take(fnavtex_rx *rx, fnavtex_message *m, char *t)
{
    return fnavtex_rx_take_message(rx, m, t, 1024);
}

static void test_framing(void)
{
    fnavtex_message m;
    char t[1024];
    fnavtex_rx *rx = make_rx(FNAVTEX_MODE_NAVTEX, 1000, 0, 0);

    /* complete message */
    feed_text(rx, "ZCZC AE07\nWIND SW 4\nNNNN");
    CHECK(take(rx, &m, t) == 1, "message logged");
    CHECK(m.origin == 'A' && m.subject == 'E' && m.number == 7, "B1B2B3B4 %c%c%d", m.origin,
          m.subject, m.number);
    CHECK(m.flags == (FNAVTEX_MSG_HEADER | FNAVTEX_MSG_END), "flags %x", m.flags);
    CHECK(strcmp(t, "WIND SW 4\n") == 0 && m.text_len == 10 && m.body_len == 10, "text %s", t);
    CHECK(strcmp(fnavtex_subject_name(m.subject), "Meteorological forecast") == 0, "subject");
    CHECK(take(rx, &m, t) == 0, "one message only");

    /* lost header */
    feed_text(rx, "NO HEADER\nNNNN");
    CHECK(take(rx, &m, t) == 1 && strcmp(t, "[Lost header]:NO HEADER\n") == 0 &&
          m.flags == FNAVTEX_MSG_END && m.origin == '?' && m.number == 0, "lost header: %s", t);

    /* lost trailer: the cut text is cleaned (navtex.cxx:702-708), the next message keeps on */
    feed_text(rx, "ZCZC AA01\nONE  TWO\nZCZC AB02\nSECOND\nNNNN");
    CHECK(take(rx, &m, t) == 1 && strcmp(t, "ONE TWO:[Lost trailer]") == 0 &&
          m.flags == (FNAVTEX_MSG_HEADER | FNAVTEX_MSG_NEW_HEADER) && m.subject == 'A',
          "lost trailer: %s", t);
    CHECK(take(rx, &m, t) == 1 && strcmp(t, "SECOND\n") == 0 && m.subject == 'B' &&
          m.number == 2, "message after the lost trailer: %s", t);

    /* garbage before a header without a message: "[Lost header]:...:[Lost trailer]" */
    feed_text(rx, "XX\n  YY ZCZC AC03\n");
    CHECK(take(rx, &m, t) == 1 && strcmp(t, "[Lost header]:XX\nYY:[Lost trailer]") == 0 &&
          m.flags == FNAVTEX_MSG_NEW_HEADER, "garbage cut: %s", t);
    /* only blanks before a header: nothing (msg_cut empty after cleanup) */

    /* timeout: 600 s after the header the message is flushed */
    feed_text(rx, "NO END");
    rx->time_sec = rx->message_time + 600;
    fnavtex_i_process_timeout(rx);
    CHECK(take(rx, &m, t) == 0, "no timeout at 600 s");
    rx->time_sec = rx->message_time + 601;
    fnavtex_i_process_timeout(rx);
    CHECK(take(rx, &m, t) == 1 && strcmp(t, "NO END:<TIMEOUT>") == 0 &&
          m.flags == (FNAVTEX_MSG_HEADER | FNAVTEX_MSG_TIMEOUT) && m.number == 3,
          "timeout: %s", t);
    /* fldigi logs an empty "[Lost header]::<TIMEOUT>" every 600 s without messages */
    rx->time_sec = rx->message_time + 601;
    fnavtex_i_process_timeout(rx);
    CHECK(take(rx, &m, t) == 1 && strcmp(t, "[Lost header]::<TIMEOUT>") == 0 &&
          m.body_len == 0, "empty timeout: %s", t);
    fnavtex_rx_destroy(rx);

    /* min_msg_len suppresses short messages */
    {
        fnavtex_rx_config c;
        memset(&c, 0, sizeof c);
        c.mode = FNAVTEX_MODE_NAVTEX;
        c.min_msg_len = 20;
        rx = fnavtex_rx_create(&c);
        feed_text(rx, "ZCZC AE07\nSHORT\nNNNN");
        CHECK(take(rx, &m, t) == 0, "short message not logged");
        feed_text(rx, "ZCZC AE08\nA MESSAGE LONGER THAN TWENTY\nNNNN");
        CHECK(take(rx, &m, t) == 1, "long message logged");
        fnavtex_rx_destroy(rx);
    }

    /* SITOR-B: no framing, no messages, no timeout */
    rx = make_rx(FNAVTEX_MODE_SITORB, 1000, 0, 0);
    feed_text(rx, "ZCZC AE07\nWIND\nNNNN");
    rx->time_sec = 10000;
    fnavtex_i_process_timeout(rx);
    CHECK(fnavtex_rx_pending(rx) == 0, "SITOR-B logs no message");
    fnavtex_rx_destroy(rx);

    /* queue overflow drops the oldest */
    rx = make_rx(FNAVTEX_MODE_NAVTEX, 1000, 0, 0);
    {
        int i;
        for (i = 0; i < 20; i++)
            feed_text(rx, "ZCZC AE07\nX\nNNNN");
        CHECK(fnavtex_rx_pending(rx) == 16 && fnavtex_rx_dropped(rx) == 4, "queue bound");
        CHECK(fnavtex_rx_take_message(rx, &m, t, 2) == 1 && t[0] == 'X' && t[1] == 0 &&
              m.text_len == 2, "truncated copy");
    }
    fnavtex_rx_destroy(rx);
}

static void test_cleanup(void)
{
    char out[64];
    size_t n = fnavtex_cleanup("\r\n  A  B\r\n\r\nC \t", 15, out, sizeof out);
    CHECK(n == 5 && strcmp(out, "A B\nC") == 0, "cleanup '%s'", out);
    n = fnavtex_cleanup("ABCDEF", 6, out, 4);
    CHECK(n == 6 && strcmp(out, "ABC") == 0, "cleanup cut");
    CHECK(strcmp(fnavtex_subject_name('Q'), "Invalid navtex subject") == 0, "subject Q");
}

/* ---------------------------------------------------------------------------------- */
/* transmitter                                                                        */
/* ---------------------------------------------------------------------------------- */

static void test_tx_shape(void)
{
    fnavtex_tx_config c;
    fnavtex_tx *tx;
    size_t n, k, i, ncodes;
    double *x, maxstep = 0, peak = 0;
    unsigned char codes[256];
    memset(&c, 0, sizeof c);
    c.mode = FNAVTEX_MODE_SITORB;
    c.carrier_hz = 1500;
    c.first_number = -1;
    tx = fnavtex_tx_create(&c);
    CHECK(fnavtex_tx_load(tx, "A", 1, 1, 1) == 0, "one code refused (create_fec reads before)");
    CHECK(fnavtex_tx_load(tx, "", 0, 1, 1) == 0, "empty refused");
    {
        char *big = (char *)malloc(FNAVTEX_TX_MAX_TEXT + 2);
        memset(big, 'E', FNAVTEX_TX_MAX_TEXT + 1);
        CHECK(fnavtex_tx_load(tx, big, FNAVTEX_TX_MAX_TEXT + 1, 1, 1) == 0, "too long refused");
        CHECK(fnavtex_tx_load(tx, big, FNAVTEX_TX_MAX_TEXT, 1, 1) > 0, "max length accepted");
        free(big);
    }
    n = fnavtex_tx_load(tx, "RYRY", 4, 1, 1);
    ncodes = fnavtex_tx_codes(tx, codes, sizeof codes);
    CHECK(ncodes == 2 * 4 + 8 && n == ncodes * 7 * 110, "SITOR-B length %zu", n);
    x = (double *)malloc(n * sizeof(double));
    k = fnavtex_tx_render(tx, x, n);
    CHECK(k == n && fnavtex_tx_remaining(tx) == 0 && fnavtex_tx_render(tx, x, n) == 0,
          "render all");
    for (i = 0; i < n; i++) {
        if (fabs(x[i]) > peak)
            peak = fabs(x[i]);
        if (i && fabs(x[i] - x[i - 1]) > maxstep)
            maxstep = fabs(x[i] - x[i - 1]);
    }
    /* phase-continuous: no step larger than one sample of the mark tone at peak 0.9 */
    CHECK(peak <= 0.9 && peak > 0.89, "peak %.4f", peak);
    CHECK(maxstep <= 0.9 * 2 * M_PI * 1585.0 / 11025 + 1e-9, "max step %.4f", maxstep);
    free(x);
    fnavtex_tx_destroy(tx);

    /* NAVTEX: phasing 10 s (first) / 5 s, 2 s after the last; header numbers count up */
    c.mode = FNAVTEX_MODE_NAVTEX;
    c.first_number = 98;
    tx = fnavtex_tx_create(&c);
    CHECK(fnavtex_tx_next_number(tx) == 98, "first number");
    n = fnavtex_tx_load(tx, "X", 1, 1, 0);
    ncodes = fnavtex_tx_codes(tx, NULL, 0);
    CHECK(n == 110250 + ncodes * 770, "first message: 10 s phasing, no trailer (%zu)", n);
    CHECK(fnavtex_tx_next_number(tx) == 99, "number 99 next");
    n = fnavtex_tx_load(tx, "X", 1, 0, 1);
    CHECK(n == 55125 + fnavtex_tx_codes(tx, NULL, 0) * 770 + 22050, "next: 5 s + 2 s (%zu)", n);
    CHECK(fnavtex_tx_next_number(tx) == 0, "counter wraps to 00");
    fnavtex_tx_destroy(tx);
}

static void test_loopback(void)
{
    static const struct { int mode; double carrier; int ita2; const char *text; const char *want; }
    cases[] = {
        {FNAVTEX_MODE_SITORB, 1000, 0, "CQ CQ DE DG6FL DG6FL K", "CQ CQ DE DG6FL DG6FL K"},
        {FNAVTEX_MODE_SITORB, 1700, 0, "THE QUICK BROWN FOX 0123456789 ?-/().,'",
         "THE QUICK BROWN FOX 0123456789 ?-/().,'"},
        {FNAVTEX_MODE_SITORB, 600, 1, "ITA2 = + '", "ITA2 = + '"},
        {FNAVTEX_MODE_NAVTEX, 1000, 0, "WIND SW 4\nSEA 2 M", "ZCZC ZI01\nWIND SW 4\nSEA 2 M\nNNNN"},
        {FNAVTEX_MODE_NAVTEX, 2200, 0, "dg6fl test de dg6fl", "ZCZC ZI01\nDG6FL TEST DE DG6FL"},
    };
    size_t c;
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        size_t n, xn;
        double *tx = render(cases[c].mode, cases[c].carrier, cases[c].ita2, cases[c].text, &n);
        double *x = with_noise(tx, n, 0.002, &xn);
        tbuf text = {NULL, 0, 0}, msgs = {NULL, 0, 0};
        decode_text(cases[c].mode, cases[c].carrier, cases[c].ita2, 0, x, xn, 1000, &text, &msgs);
        CHECK(text.p && contains(text.p, text.n, cases[c].want), "loopback %zu: '%s'", c,
              text.p ? text.p : "");
        if (cases[c].mode == FNAVTEX_MODE_NAVTEX)
            CHECK(msgs.n > 0, "loopback %zu: message logged", c);
        free(tx);
        free(x);
        free(text.p);
        free(msgs.p);
    }
}

static void test_chunk_invariance(void)
{
    /* AC-890.5: the same output for any chunking of the input */
    size_t n, xn, c;
    double *tx = render(FNAVTEX_MODE_NAVTEX, 1000, 0, "CHUNK TEST 12345", &n);
    double *x = with_noise(tx, n, 0.05, &xn);
    static const size_t chunks[] = {1, 7, 511, 512, 513, 4096, 100000};
    tbuf ref = {NULL, 0, 0}, refm = {NULL, 0, 0};
    uint64_t refh = 0;
    size_t refn = 0;
    for (c = 0; c < sizeof chunks / sizeof chunks[0]; c++) {
        tbuf text = {NULL, 0, 0}, msgs = {NULL, 0, 0};
        uint64_t h;
        size_t bn;
        fnavtex_rx *rx = make_rx(FNAVTEX_MODE_NAVTEX, 1000, 0, 0);
        decode_all(rx, x, xn, chunks[c], &text, &msgs, &h, &bn);
        if (c == 0) {
            ref = text;
            refm = msgs;
            refh = h;
            refn = bn;
            CHECK(text.p && contains(text.p, text.n, "CHUNK TEST 12345"), "chunk decode");
        } else {
            CHECK(text.n == ref.n && memcmp(text.p, ref.p, ref.n) == 0, "chunk %zu text",
                  chunks[c]);
            CHECK(msgs.n == refm.n && memcmp(msgs.p, refm.p, refm.n) == 0, "chunk %zu msgs",
                  chunks[c]);
            CHECK(h == refh && bn == refn, "chunk %zu bits", chunks[c]);
            free(text.p);
            free(msgs.p);
        }
        fnavtex_rx_destroy(rx);
    }
    /* reset gives the fresh receiver's output */
    {
        fnavtex_rx *rx = make_rx(FNAVTEX_MODE_NAVTEX, 1000, 0, 0);
        tbuf t1 = {NULL, 0, 0}, t2 = {NULL, 0, 0};
        decode_all(rx, x, xn / 2, 4096, &t1, NULL, NULL, NULL);
        fnavtex_rx_reset(rx);
        decode_all(rx, x, xn, 4096, &t2, NULL, NULL, NULL);
        CHECK(t2.n == ref.n && memcmp(t2.p, ref.p, ref.n) == 0, "reset");
        fnavtex_rx_destroy(rx);
        free(t1.p);
        free(t2.p);
    }
    free(ref.p);
    free(refm.p);
    free(tx);
    free(x);
}

static void test_noise_no_text(void)
{
    /* 120 s of noise: no message with a header and (almost) no text in both modes */
    int mode;
    size_t n = 120 * FNAVTEX_RATE, i;
    double *x = (double *)malloc(n * sizeof(double));
    for (i = 0; i < n; i++)
        x[i] = 0.1 * gauss();
    for (mode = 0; mode < 2; mode++) {
        tbuf text = {NULL, 0, 0}, msgs = {NULL, 0, 0};
        decode_text(mode, 1000, 0, 0, x, n, 4096, &text, &msgs);
        CHECK(text.n <= 10, "mode %d: %zu bytes from noise", mode, text.n);
        CHECK(msgs.n == 0 || !contains(msgs.p, msgs.n, " 0 "), "mode %d: messages", mode);
        free(text.p);
        free(msgs.p);
    }
    free(x);
}

static void test_sample_clock_64bit(void)
{
    /* fldigi's int sample counter wraps after 2^31 samples (2.25 days at 11025 Hz); the
     * 64-bit counter decodes across that point */
    size_t n, xn;
    double *tx = render(FNAVTEX_MODE_SITORB, 1000, 0, "PAST TWO DAYS", &n);
    double *x = with_noise(tx, n, 0.001, &xn);
    fnavtex_rx *rx = make_rx(FNAVTEX_MODE_SITORB, 1000, 0, 0);
    tbuf text = {NULL, 0, 0};
    double off = (double)INT_MAX - 2.0 * FNAVTEX_RATE;
    /* jump the clock (and the bit-sync events with it) to just before INT_MAX, keeping the
     * phase within the 882-sample multicorrelator period */
    off -= fmod(off, 882.0);
    rx->sample_count = (int64_t)off;
    rx->next_early_event += off;
    rx->next_prompt_event += off;
    rx->next_late_event += off;
    decode_all(rx, x, xn, 4096, &text, NULL, NULL, NULL);
    CHECK(fnavtex_rx_samples(rx) > (long long)INT_MAX, "counter beyond INT_MAX");
    CHECK(text.p && contains(text.p, text.n, "PAST TWO DAYS"), "decodes past 2^31: '%s'",
          text.p ? text.p : "");
    fnavtex_rx_destroy(rx);
    free(text.p);
    free(tx);
    free(x);
}

static void test_afc(void)
{
    /* a signal 50 Hz above the receiver: with AFC (fldigi's algorithm on the core's power
     * spectrum: jumps of +-85 Hz while phasing, 3 Hz steps while reading data) the carrier
     * ends up closer to the signal and the text decodes */
    size_t n, xn;
    double *tx = render(FNAVTEX_MODE_NAVTEX, 1050, 0, "AFC PULLS THE CARRIER IN", &n);
    double *x = with_noise(tx, n, 0.01, &xn);
    fnavtex_rx *rx = make_rx(FNAVTEX_MODE_NAVTEX, 1000, 0, 1);
    tbuf on = {NULL, 0, 0}, off = {NULL, 0, 0};
    decode_all(rx, x, xn, 4096, &on, NULL, NULL, NULL);
    CHECK(fabs(fnavtex_rx_carrier_hz(rx) - 1050) < 25, "AFC carrier %.1f",
          fnavtex_rx_carrier_hz(rx));
    CHECK(on.p && contains(on.p, on.n, "AFC PULLS THE CARRIER IN"), "AFC decode '%s'",
          on.p ? on.p : "");
    fnavtex_rx_destroy(rx);
    rx = make_rx(FNAVTEX_MODE_NAVTEX, 1000, 0, 0);
    decode_all(rx, x, xn, 4096, &off, NULL, NULL, NULL);
    CHECK(fnavtex_rx_carrier_hz(rx) == 1000, "no AFC: carrier fixed");
    fnavtex_rx_destroy(rx);
    free(on.p);
    free(off.p);
    free(tx);
    free(x);
}

static void test_snr_estimate(void)
{
    size_t n, xn;
    double *tx = render(FNAVTEX_MODE_SITORB, 1000, 0, "SNR SNR SNR SNR SNR SNR SNR SNR", &n);
    /* fldigi's s/n compares the mark and space densities with 240 x the density at the
     * centre, where the FSK sidebands sit: a clean signal reads about -15 dB, noise pulls
     * it down. Noise rms 0.002 (+48 dB in 2500 Hz) against 0.5 (-12 dB). */
    double *strong = with_noise(tx, n, 0.002, &xn);
    double *weak = with_noise(tx, n, 0.5, &xn);
    fnavtex_rx *rx = make_rx(FNAVTEX_MODE_SITORB, 1000, 0, 0);
    double s1, s2;
    fnavtex_rx_process(rx, strong, FNAVTEX_RATE + n / 2, NULL, 0);
    s1 = fnavtex_rx_snr_db(rx);
    fnavtex_rx_reset(rx);
    fnavtex_rx_process(rx, weak, FNAVTEX_RATE + n / 2, NULL, 0);
    s2 = fnavtex_rx_snr_db(rx);
    CHECK(s1 > s2 + 1 && s1 > -20 && s1 < -10, "s/n strong %.1f dB, weak %.1f dB", s1, s2);
    CHECK(fnavtex_rx_metric(rx) >= 0 && fnavtex_rx_metric(rx) <= 100, "metric range");
    fnavtex_rx_destroy(rx);
    free(tx);
    free(strong);
    free(weak);
}

static void test_occupied_bandwidth(void)
{
    /* 99 % power bandwidth of a SITOR-B burst: fldigi's phase-continuous 170 Hz FSK with
     * hard keying at 100 baud */
    size_t n, i, len = 1 << 17;
    double *tx = render(FNAVTEX_MODE_SITORB, 1500, 0,
                        "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 RYRYRYRY", &n);
    fdsp_gfft fft;
    fdsp_cmpx *buf = (fdsp_cmpx *)calloc(len, sizeof(fdsp_cmpx));
    double *p = (double *)malloc(len / 2 * sizeof(double)), total = 0, acc = 0;
    double lo = 0, hi = 0, bw;
    fdsp_gfft_init(&fft, len);
    for (i = 0; i < n && i < len; i++)
        buf[i].re = tx[i];
    fdsp_gfft_forward(&fft, buf);
    for (i = 0; i < len / 2; i++) {
        p[i] = buf[i].re * buf[i].re + buf[i].im * buf[i].im;
        total += p[i];
    }
    for (i = 0; i < len / 2; i++) {
        acc += p[i];
        if (lo == 0 && acc >= 0.005 * total)
            lo = (double)i * FNAVTEX_RATE / len;
        if (acc >= 0.995 * total) {
            hi = (double)i * FNAVTEX_RATE / len;
            break;
        }
    }
    bw = hi - lo;
    printf("SITOR-B 99 %% occupied bandwidth: %.1f Hz (%.1f .. %.1f Hz)\n", bw, lo, hi);
    CHECK(bw > 170 && bw < 400, "occupied bandwidth %.1f Hz", bw);
    CHECK(lo > 1500 - 200 && hi < 1500 + 200, "band edges %.1f .. %.1f", lo, hi);
    fdsp_gfft_free(&fft);
    free(buf);
    free(p);
    free(tx);
}

int main(void)
{
    test_version();
    test_create_refusals();
    test_ccir476();
    test_encoder();
    test_fec_cascade();
    test_find_alpha();
    test_framing();
    test_cleanup();
    test_tx_shape();
    test_loopback();
    test_chunk_invariance();
    test_noise_no_text();
    test_sample_clock_64bit();
    test_afc();
    test_snr_estimate();
    test_occupied_bandwidth();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("fldigi-navtex-core: all tests passed (%s)\n", fnavtex_version());
    return 0;
}
