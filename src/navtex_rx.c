/*
 * fldigi-navtex-core: the NAVTEX / SITOR-B receiver of fldigi 4.2.13
 * (src/navtex/navtex.cxx, class navtex_implementation and class ccir_message).
 *
 * Copyright (C) 2026 Gerolf Ziegenhain
 * Original C++: (c) 2011-2016 Remi Chateauneu F4ECW, Rik van Riel AB1KW; JNX (c) Paul Lutus;
 * waterfall power spectrum (c) Dave Freese W1HKJ.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every function follows the fldigi function it is named after, statement for statement.
 * Floating point is evaluated in fldigi's order: std::complex products as four separate
 * products, |z| as hypot, cos/sin pairs of one argument as one sincos call, and the three
 * multiply-adds that fldigi's reference build (Apple clang -O2, FP contraction on) fuses in
 * the discriminator written out as fma(). Deliberate deviations from fldigi, each tested:
 *   - per-instance state instead of fldigi's function-static variables;
 *   - a 64-bit sample counter instead of fldigi's int, which wraps after 2.25 days;
 *   - a power spectrum computed here instead of fldigi's GUI waterfall (AFC, s/n);
 *   - USB only: fldigi's waterfall "reverse" and the modem reverse flag are not ported;
 *   - logic levels outside the int range saturate and the FEC sums wrap, which is what
 *     fldigi's double-to-int conversion and int additions do on its reference build
 *     (arm64), where C leaves them undefined.
 */
#include "navtex_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------- */
/* helpers                                                                           */
/* ---------------------------------------------------------------------------------- */

static double decayavg(double average, double input, int weight)
{
    /* include/misc.h:59-63 */
    if (weight <= 1)
        return input;
    return ((input - average) / (double)weight) + average;
}

static int to_int(double v)
{
    /* double -> int as fldigi's reference build converts it (arm64 fcvtzs: truncation,
     * saturation outside the int range, NaN -> 0) */
    if (v != v)
        return 0;
    if (v >= 2147483648.0)
        return INT_MAX;
    if (v <= -2147483649.0)
        return INT_MIN;
    return (int)v;
}

static int wrap_add(int a, int b)
{
    /* int addition with two's complement wrap-around, as on fldigi's reference build */
    return (int)((unsigned int)a + (unsigned int)b);
}

static int wrap_neg(int a)
{
    return (int)(0u - (unsigned int)a);
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int is_alnum(int c)
{
    return is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static void emit_char(fnavtex_rx *rx, int c)
{
    if (rx->ev && rx->ev_n < rx->ev_max) {
        rx->ev[rx->ev_n].ch = (unsigned char)c;
        rx->ev[rx->ev_n].flags = rx->char_from_fec ? FNAVTEX_EV_FEC : 0;
        rx->ev_n++;
    } else {
        rx->dropped++;
    }
}

/* ---------------------------------------------------------------------------------- */
/* power spectrum (in place of fldigi's waterfall)                                   */
/* ---------------------------------------------------------------------------------- */

static void wf_update(fnavtex_rx *rx)
{
    /* WFdisp::processFFT, waterfall.cxx:599-638, on the receiver's own samples at 11025 Hz:
     * Blackman window (BlackmanWindow, misc.h:128-138, fldigi's default wfPreFilter), scale
     * 2 / WF_FFTLEN, power norm(X) of the bin nearest to each 1 Hz column. */
    const double vscale = 2.0 / NAVTEX_WF_FFTLEN;
    const double scale = (double)NAVTEX_WF_FFTLEN / FNAVTEX_RATE;
    int i;
    for (i = 0; i < NAVTEX_WF_FFTLEN; i++) {
        int k = (rx->wf_pos + i) % NAVTEX_WF_FFTLEN;
        rx->wf_buf[i].re = rx->wf_window[i] * rx->wf_ring[k] * vscale;
        rx->wf_buf[i].im = 0.0;
    }
    fdsp_gfft_forward(&rx->wfft, rx->wf_buf);
    rx->pwr[NAVTEX_LOW_CUTOFF] = 0.0;
    for (i = NAVTEX_LOW_CUTOFF + 1; i <= NAVTEX_WF_WIDTH; i++) {
        int n = (int)floor(scale * i + 0.5);
        double re = rx->wf_buf[n].re, im = rx->wf_buf[n].im;
        rx->pwr[i] = re * re + im * im;
    }
}

static double power_density(const fnavtex_rx *rx, double f0, double bw)
{
    /* WFdisp::powerDensity, waterfall.cxx:520-533 */
    double pwrdensity = 0.0;
    int flower = (int)((f0 - bw / 2)),
        fupper = (int)((f0 + bw / 2));
    int i;
    if (flower < 0 || fupper > NAVTEX_WF_WIDTH)
        return 0.0;
    for (i = flower; i <= fupper; i++)
        pwrdensity += rx->pwr[i];
    return pwrdensity / (bw + 1);
}

static double power_density_maximum(const fnavtex_rx *rx, int bw_nb, const int (*bw)[2])
{
    /* WFdisp::powerDensityMaximum, waterfall.cxx:536-576 (bw_nb is 2 here) */
    int fmax[2];
    double pnbw[2];
    int f_lowest, f_highest, i, n, fmid = 0;
    double max_pwr, total_pwr = 0;
    for (i = 0; i < bw_nb; i++) {
        fmax[i] = rx->wf_carrier;
        pnbw[i] = 0;
    }
    for (i = 0; i < bw_nb; i++) {
        f_lowest = rx->wf_carrier + bw[i][0];
        if (f_lowest <= 0)
            f_lowest = 0;
        f_highest = rx->wf_carrier + bw[i][1];
        if (f_highest > NAVTEX_WF_WIDTH)
            f_highest = NAVTEX_WF_WIDTH;
        max_pwr = 0;
        pnbw[i] = 0;
        for (n = f_lowest; n < f_highest; n++) {
            if (rx->pwr[n] > max_pwr) {
                max_pwr = rx->pwr[n];
                fmax[i] = n;
            }
            pnbw[i] += rx->pwr[n];
        }
        if (pnbw[i] == 0)
            return rx->wf_carrier;
    }
    for (i = 0; i < bw_nb; i++)
        total_pwr += pnbw[i];
    for (i = 0; i < bw_nb; i++)
        fmid += fmax[i] * pnbw[i] / total_pwr;
    return fmid;
}

/* ---------------------------------------------------------------------------------- */
/* messages: ccir_message (navtex.cxx:624-858) and display_message (:1683-1698)       */
/* ---------------------------------------------------------------------------------- */

static void msg_reset(fnavtex_rx *rx)
{
    /* ccir_message::reset_msg / init_members, navtex.cxx:692-713 */
    rx->msg_len = 0;
    rx->origin = '?';
    rx->subject = '?';
    rx->number = 0;
    rx->msg_fec = 0;
    rx->msg_failed = 0;
}

static void display_message(fnavtex_rx *rx, size_t body_len, char origin, char subject,
                            int number, const char *pre, const char *body, size_t n,
                            const char *post, int flags)
{
    /* display_message, navtex.cxx:1683-1698: messages shorter than NVTX_MinSizLoggedMsg are
     * not logged; the logged text is what put_received_message queues for XML-RPC. */
    navtex_qmsg *q;
    size_t lp = strlen(pre), lq = strlen(post), slot;
    if (body_len < (size_t)rx->min_msg_len)
        return;
    if (rx->q_count == NAVTEX_QUEUE) {
        rx->q_head = (rx->q_head + 1) % NAVTEX_QUEUE;
        rx->q_count--;
        rx->dropped++;
    }
    slot = (rx->q_head + rx->q_count) % NAVTEX_QUEUE;
    q = &rx->queue[slot];
    memcpy(q->text, pre, lp);
    memcpy(q->text + lp, body, n);
    memcpy(q->text + lp + n, post, lq);
    q->text[lp + n + lq] = '\0';
    q->m.origin = origin;
    q->m.subject = subject;
    q->m.number = number;
    q->m.flags = flags;
    q->m.body_len = body_len;
    q->m.text_len = lp + n + lq;
    q->m.fec_chars = rx->msg_fec;
    q->m.failed_chars = rx->msg_failed;
    q->m.end_sample = (long long)rx->sample_count;
    rx->q_count++;
}

static void flush_message(fnavtex_rx *rx, const char *extra_info, int end_flag)
{
    /* flush_message, navtex.cxx:1016-1029 */
    if (rx->header_found) {
        rx->header_found = 0;
        display_message(rx, rx->msg_len, rx->origin, rx->subject, rx->number, "", rx->msg,
                        rx->msg_len, extra_info, FNAVTEX_MSG_HEADER | end_flag);
    } else {
        display_message(rx, rx->msg_len, rx->origin, rx->subject, rx->number,
                        "[Lost header]:", rx->msg, rx->msg_len, extra_info, end_flag);
    }
    msg_reset(rx);
    rx->message_time = rx->time_sec;
}

void fnavtex_i_process_timeout(fnavtex_rx *rx)
{
    /* process_timeout, navtex.cxx:1032-1043 */
    if (rx->only_sitor_b)
        return;
    if (!(rx->time_sec - rx->message_time > NAVTEX_TIMEOUT_S))
        return;
    flush_message(rx, ":<TIMEOUT>", FNAVTEX_MSG_TIMEOUT);
}

static int detect_end(fnavtex_rx *rx)
{
    /* ccir_message::detect_end, navtex.cxx:753-768: "NNNN" at the end is removed */
    if (rx->msg_len < 4)
        return 0;
    if (memcmp(rx->msg + rx->msg_len - 4, "NNNN", 4) != 0)
        return 0;
    rx->msg_len -= 4;
    return 1;
}

static void process_messages(fnavtex_rx *rx, int c)
{
    /* process_messages, navtex.cxx:1045-1078 */
    const char *comp;
    size_t qlen;
    if (rx->msg_len == NAVTEX_MSG_MAX) {
        /* only reachable in SITOR-B mode, where the string is never shown */
        memmove(rx->msg, rx->msg + NAVTEX_MSG_MAX / 2, NAVTEX_MSG_MAX / 2);
        rx->msg_len = NAVTEX_MSG_MAX / 2;
    }
    rx->msg[rx->msg_len++] = (char)c;

    /* No header nor trailer for plain SitorB. */
    if (rx->only_sitor_b) {
        rx->header_found = 1;
        rx->message_time = rx->time_sec;
        return;
    }

    /* ccir_message::detect_header, navtex.cxx:716-751 */
    qlen = rx->msg_len;
    comp = qlen >= NAVTEX_HEADER_LEN ? rx->msg + qlen - NAVTEX_HEADER_LEN : rx->msg;
    if (qlen >= NAVTEX_HEADER_LEN &&
        comp[0] == 'Z' && comp[1] == 'C' && comp[2] == 'Z' && comp[3] == 'C' &&
        comp[4] == ' ' && is_alnum((unsigned char)comp[5]) &&
        is_alnum((unsigned char)comp[6]) && is_digit((unsigned char)comp[7]) &&
        is_digit((unsigned char)comp[8]) && (comp[9] == '\n' || comp[9] == '\r')) {
        /* The garbage before the valid header goes out as msg_cut, a ccir_message built
         * from substr(0, size - header_len) with the old B1/B2/B3B4; its constructor runs
         * cleanup() (navtex.cxx:702-708), so the cut text and its emptiness test are on the
         * cleaned string. */
        size_t cut = fnavtex_cleanup(rx->msg, qlen - NAVTEX_HEADER_LEN, rx->scratch,
                                     NAVTEX_MSG_MAX + 1);
        char o = rx->origin, s = rx->subject;
        int num = rx->number;
        if (rx->header_found) {
            display_message(rx, cut, o, s, num, "", rx->scratch, cut, ":[Lost trailer]",
                            FNAVTEX_MSG_HEADER | FNAVTEX_MSG_NEW_HEADER);
        } else if (cut != 0) {
            display_message(rx, cut, o, s, num, "[Lost header]:", rx->scratch, cut,
                            ":[Lost trailer]", FNAVTEX_MSG_NEW_HEADER);
        }
        rx->origin = comp[5];
        rx->subject = comp[6];
        rx->number = (comp[7] - '0') * 10 + (comp[8] - '0');
        rx->msg_len = 0;
        rx->msg_fec = 0;
        rx->msg_failed = 0;
        rx->header_found = 1;
        rx->message_time = rx->time_sec;
    } else if (detect_end(rx)) {
        flush_message(rx, "", FNAVTEX_MSG_END);
    }
}

/* ---------------------------------------------------------------------------------- */
/* characters: process_char (navtex.cxx:1325-1362), filter_print (:1364-1371)          */
/* ---------------------------------------------------------------------------------- */

static void filter_print(fnavtex_rx *rx, int c)
{
    if (c == NAVTEX_CHAR_BELL) {
        /* TODO in fldigi: should be a beep, but French navtex displays a quote. */
        emit_char(rx, '\'');
    } else if (c != -1 && c != '\r' && c != NAVTEX_CODE_ALPHA && c != NAVTEX_CODE_REP) {
        emit_char(rx, c);
    }
}

void fnavtex_i_process_char(fnavtex_rx *rx, int chr)
{
    /* process_char, navtex.cxx:1325-1362 (last_char is per instance here) */
    switch (chr) {
    case NAVTEX_CODE_REP:
        /* two rep characters in alpha phase: fix the rep/alpha phase so FEC works again */
        if (rx->last_char == NAVTEX_CODE_REP)
            rx->alpha_phase = 0;
        break;
    case NAVTEX_CODE_ALPHA:
    case NAVTEX_CODE_BETA:
    case NAVTEX_CODE_CHAR32:
        break;
    case NAVTEX_CODE_LTRS:
        rx->shift = 0;
        break;
    case NAVTEX_CODE_FIGS:
        rx->shift = 1;
        break;
    default:
        chr = fnavtex_code_to_char(chr, rx->shift, rx->ita2);
        if (chr >= 0) {
            if (rx->char_from_fec)
                rx->msg_fec++;
            filter_print(rx, chr);
            process_messages(rx, chr);
        }
        break;
    }
    rx->last_char = chr;
}

/* ---------------------------------------------------------------------------------- */
/* FEC cascade: fec_offset, flip_smallest_bit, find_alpha_characters, handle_bit_value, */
/* process_bytes (navtex.cxx:1080-1323)                                               */
/* ---------------------------------------------------------------------------------- */

static int fec_offset(int offset)
{
    /* The rep character is transmitted 5 characters (35 bits) ahead of the alpha one. */
    return offset - 35;
}

void fnavtex_i_flip_smallest_bit(int *pos)
{
    /* flip_smallest_bit, navtex.cxx:1088-1118 (count_one starts at 1, as in fldigi). fldigi
     * indexes pos[-1] when every candidate sits at INT_MIN / INT_MAX; skipped here. */
    int min_zero = INT_MIN, min_one = INT_MAX;
    int min_zero_pos = -1, min_one_pos = -1;
    int count_zero = 0, count_one = 1;
    int val, i;
    for (i = 0; i < 7; i++) {
        val = pos[i];
        if (val < 0) {
            count_zero++;
            if (val > min_zero) {
                min_zero = val;
                min_zero_pos = i;
            }
        } else {
            count_one++;
            if (val < min_one) {
                min_one = val;
                min_one_pos = i;
            }
        }
    }
    if (count_zero == 4) {
        if (min_zero_pos >= 0)
            pos[min_zero_pos] = wrap_neg(pos[min_zero_pos]);
    } else if (count_one == 5) {
        if (min_one_pos >= 0)
            pos[min_one_pos] = wrap_neg(pos[min_one_pos]);
    }
}

int fnavtex_i_find_alpha_characters(const fnavtex_rx *rx)
{
    /* find_alpha_characters, navtex.cxx:1129-1187 */
    int best_offset = 0;
    int best_score = 0;
    int offset, i;
    for (offset = 35; offset < (35 + 14); offset++) {
        int score = 0;
        int reps = 0;
        int limit = NAVTEX_BITS - 7;
        for (i = offset; i < limit; i += 7) {
            if (fnavtex_valid_char_at(&rx->bit_values[i])) {
                int ri = fec_offset(i);
                int code = fnavtex_bytes_to_code(&rx->bit_values[i]);
                int rep = fnavtex_bytes_to_code(&rx->bit_values[ri]);
                score++;
                if (code == rep) {
                    /* this offset is wrong, rep and alpha are spaced odd */
                    if (code == NAVTEX_CODE_ALPHA || code == NAVTEX_CODE_REP) {
                        score = 0;
                        continue;
                    }
                    reps++;
                } else if (code == NAVTEX_CODE_ALPHA) {
                    /* is there a matching rep to this alpha? */
                    int ri2 = i - 7;
                    int rep2 = fnavtex_bytes_to_code(&rx->bit_values[ri2]);
                    if (rep2 == NAVTEX_CODE_REP)
                        reps++;
                }
            }
        }
        /* the most valid characters, with at least 3 FEC reps */
        if (reps >= 3 && score + reps > best_score) {
            best_score = score + reps;
            best_offset = offset;
        }
    }
    /* m_bit_values fits 14 characters; with at least 9 good ones tell where they start */
    if (best_score > 8)
        return best_offset;
    return -1;
}

static void set_state(fnavtex_rx *rx, int s)
{
    /* set_state, navtex.cxx:1008-1013 (the label is GUI only) */
    if (s != rx->state)
        rx->state = s;
}

int fnavtex_i_process_bytes(fnavtex_rx *rx, int cursor)
{
    /* process_bytes, navtex.cxx:1238-1323: 1 alpha decoded, 0 unmodified FEC replacement,
     * -1 soft failure (FEC calculation), -2 hard failure */
    int code = fnavtex_bytes_to_code(&rx->bit_values[cursor]);
    int success = 0;
    int i, calc, avg[7], reppos, rep;

    rx->char_from_fec = 0;
    if (fnavtex_check_bits(code)) {
        success = 1;
        goto decode;
    }
    if (fec_offset(cursor) < 0)
        return -1;

    /* The alpha character was not correct: try the rep copy and some permutations. */
    reppos = fec_offset(cursor);
    rep = fnavtex_bytes_to_code(&rx->bit_values[reppos]);
    rx->char_from_fec = 1;
    if (fnavtex_check_bits(rep)) {
        /* Current code is probably code_alpha. Skip decoding to avoid switching phase. */
        if (rep == NAVTEX_CODE_REP) {
            rx->char_from_fec = 0;
            return 0;
        }
        code = rep;
        goto decode;
    }

    /* Neither alpha nor rep are valid: is the average of the two a valid character? */
    for (i = 0; i < 7; i++) {
        int a = rx->bit_values[cursor + i];
        int r = rx->bit_values[reppos + i];
        avg[i] = wrap_add(a, r);
    }
    calc = fnavtex_bytes_to_code(avg);
    if (fnavtex_check_bits(calc)) {
        code = calc;
        success = -1;
        goto decode;
    }

    /* Flip the lowest confidence bit in alpha. */
    fnavtex_i_flip_smallest_bit(&rx->bit_values[cursor]);
    calc = fnavtex_bytes_to_code(&rx->bit_values[cursor]);
    if (fnavtex_check_bits(calc)) {
        code = calc;
        success = -1;
        goto decode;
    }

    /* Flip the lowest confidence bit in rep. */
    fnavtex_i_flip_smallest_bit(&rx->bit_values[reppos]);
    calc = fnavtex_bytes_to_code(&rx->bit_values[reppos]);
    if (fnavtex_check_bits(calc)) {
        code = calc;
        success = -1;
        goto decode;
    }

    /* Flip the bit with the lowest confidence in the average of alpha and rep. */
    fnavtex_i_flip_smallest_bit(avg);
    calc = fnavtex_bytes_to_code(avg);
    if (fnavtex_check_bits(calc)) {
        code = calc;
        success = -1;
        goto decode;
    }

    rx->char_from_fec = 0;
    rx->msg_failed++;
    return -2;

decode:
    fnavtex_i_process_char(rx, code);
    rx->char_from_fec = 0;
    return success;
}

void fnavtex_i_handle_bit_value(fnavtex_rx *rx, int logic_level)
{
    /* handle_bit_value, navtex.cxx:1191-1230 */
    int buffersize = NAVTEX_BITS;
    int i, offset = 0;
    size_t slot;

    slot = (rx->tap_head + rx->tap_count) % NAVTEX_BIT_TAP;
    rx->tap_samples[slot] = (long long)rx->sample_count;
    rx->tap_levels[slot] = logic_level;
    if (rx->tap_count < NAVTEX_BIT_TAP)
        rx->tap_count++;
    else
        rx->tap_head = (rx->tap_head + 1) % NAVTEX_BIT_TAP;

    /* store the received value in the bit stream */
    for (i = 0; i < buffersize - 1; i++)
        rx->bit_values[i] = rx->bit_values[i + 1];
    rx->bit_values[buffersize - 1] = logic_level;
    if (rx->bit_cursor > 0)
        rx->bit_cursor--;

    /* find the most likely location where the message starts */
    if (rx->state == FNAVTEX_STATE_SYNC) {
        offset = fnavtex_i_find_alpha_characters(rx);
        if (offset >= 0) {
            set_state(rx, FNAVTEX_STATE_READ_DATA);
            rx->bit_cursor = offset;
            rx->alpha_phase = 1;
        } else {
            set_state(rx, FNAVTEX_STATE_SYNC_SETUP);
        }
    }

    /* process 7-bit characters as they come in, skipping the rep (duplicate) ones */
    if (rx->state == FNAVTEX_STATE_READ_DATA) {
        if (rx->bit_cursor < buffersize - 7) {
            if (rx->alpha_phase) {
                int ret = fnavtex_i_process_bytes(rx, rx->bit_cursor);
                rx->error_count -= ret;
                if (rx->error_count > 5)
                    set_state(rx, FNAVTEX_STATE_SYNC_SETUP);
                if (rx->error_count < 0)
                    rx->error_count = 0;
            }
            rx->alpha_phase = !rx->alpha_phase;
            rx->bit_cursor += 7;
        }
    }
}

/* ---------------------------------------------------------------------------------- */
/* metric and AFC (navtex.cxx:1373-1464)                                              */
/* ---------------------------------------------------------------------------------- */

static void compute_metric(fnavtex_rx *rx)
{
    /* compute_metric, navtex.cxx:1373-1393 */
    double delta = rx->baud_rate / 8.0;
    double np = power_density(rx, rx->center_f, delta) * 3000 / delta;
    double sp =
        power_density(rx, rx->mark_f, delta) +
        power_density(rx, rx->space_f, delta) + 1e-10;
    double v;
    rx->sigpwr = decayavg(rx->sigpwr, sp, sp > rx->sigpwr ? 2 : 8);
    rx->noisepwr = decayavg(rx->noisepwr, np, 16);
    rx->snr = 10 * log10(rx->sigpwr / rx->noisepwr);
    v = (3000 / delta) * (rx->sigpwr / rx->noisepwr);
    rx->metric = v < 0.0 ? 0.0 : v > 100.0 ? 100.0 : v;
}

static void set_filter_values(fnavtex_rx *rx)
{
    /* set_filter_values, navtex.cxx:987-992 */
    rx->mark_f = rx->center_f + FNAVTEX_SHIFT_HZ;
    rx->space_f = rx->center_f - FNAVTEX_SHIFT_HZ;
    rx->mark_phase = 0;
    rx->space_phase = 0;
}

static void set_freq(fnavtex_rx *rx, double freq)
{
    /* navtex::set_freq, navtex.cxx:1995-1999: modem::set_freq clamps the modem frequency
     * (modem.cxx:319-333) and moves the waterfall cursor (put_freq, fl_digi.cxx:9122-9125,
     * WFdisp::carrier, waterfall.cxx:903-908); the navtex carrier itself takes the value
     * unclamped (set_carrier, navtex.cxx:1905-1909). */
    double fr = freq;
    int cf;
    if (fr < NAVTEX_LOW_CUTOFF + FNAVTEX_BANDWIDTH_HZ / 2.0)
        fr = NAVTEX_LOW_CUTOFF + FNAVTEX_BANDWIDTH_HZ / 2.0;
    if (fr > NAVTEX_HIGH_CUTOFF - FNAVTEX_BANDWIDTH_HZ / 2.0)
        fr = NAVTEX_HIGH_CUTOFF - FNAVTEX_BANDWIDTH_HZ / 2.0;
    cf = (int)floor(fr + 0.5);
    if (cf >= 32 / 2 && cf < (NAVTEX_WF_WIDTH - 32 / 2))
        rx->wf_carrier = cf;
    rx->center_f = freq;
    set_filter_values(rx);
}

static void process_afc(fnavtex_rx *rx)
{
    /* process_afc, navtex.cxx:1395-1464 */
    static const int bw[][2] = {
        {-FNAVTEX_SHIFT_HZ - 10, -FNAVTEX_SHIFT_HZ + 5},
        {FNAVTEX_SHIFT_HZ - 5, FNAVTEX_SHIFT_HZ + 10}};
    const size_t delay_upd = 50;
    double max_carrier, next_carr = 0.0, delta;
    int lingering_state;
    if (!rx->afc)
        return;
    ++rx->cnt_upd;
    /* AFC from time to time */
    if ((rx->cnt_upd % delay_upd) != 0)
        return;
    /* centre the carrier where the activity is the strongest */
    max_carrier = power_density_maximum(rx, 2, bw);
    if (rx->state == FNAVTEX_STATE_READ_DATA) {
        rx->cnt_read_data = (int)(delay_upd / 20);
        lingering_state = FNAVTEX_STATE_READ_DATA;
    } else {
        if (rx->cnt_read_data) {
            --rx->cnt_read_data;
            lingering_state = FNAVTEX_STATE_READ_DATA;
        } else {
            double pwr_left, pwr_right;
            const double ratio_left_right = 5.0;
            lingering_state = rx->state;
            /* maybe this is the phasing signal, so we recenter */
            pwr_left = power_density(rx, max_carrier - FNAVTEX_SHIFT_HZ, 30);
            pwr_right = power_density(rx, max_carrier + FNAVTEX_SHIFT_HZ, 30);
            if (pwr_left > ratio_left_right * pwr_right)
                max_carrier -= FNAVTEX_SHIFT_HZ;
            else if (ratio_left_right * pwr_left < pwr_right)
                max_carrier += FNAVTEX_SHIFT_HZ;
        }
    }
    switch (lingering_state) {
    case FNAVTEX_STATE_SYNC_SETUP:
        next_carr = max_carrier;
        break;
    case FNAVTEX_STATE_SYNC:
        next_carr = decayavg(rx->center_f, max_carrier, 1);
        break;
    case FNAVTEX_STATE_READ_DATA:
        /* it will stay stable for a couple of calls */
        if (max_carrier < rx->center_f)
            next_carr = fmax(max_carrier, rx->center_f - 3.0);
        else if (max_carrier > rx->center_f)
            next_carr = fmin(max_carrier, rx->center_f + 3.0);
        else
            next_carr = max_carrier;
        break;
    default:
        break;
    }
    delta = fabs(rx->center_f - next_carr);
    if (delta > 1.0)
        set_freq(rx, next_carr);
}

/* ---------------------------------------------------------------------------------- */
/* bit sync and demodulation                                                          */
/* ---------------------------------------------------------------------------------- */

static void process_multicorrelator(fnavtex_rx *rx)
{
    /* process_multicorrelator, navtex.cxx:1480-1505: adjust the sampling once every 8 bits */
    double slope;
    if (rx->sample_count % (int)(rx->bit_sample_count * 8))
        return;
    /* slope between early and late signals, usually -1 .. 1 */
    slope = rx->average_late_signal - rx->average_early_signal;
    slope /= (rx->average_prompt_signal + 0.1);
    /* scale by the distance between the sample points */
    slope *= 22;
    /* slow down the adjustment at the peak */
    if (rx->average_prompt_signal > rx->average_early_signal &&
        rx->average_prompt_signal > rx->average_late_signal)
        slope /= 2;
    if (slope) {
        rx->next_early_event += slope;
        rx->next_prompt_event += slope;
        rx->next_late_event += slope;
    }
}

static double noise_decay(const fnavtex_rx *rx, double avg, double value)
{
    /* noise average decays fast down, slow up (navtex.cxx:1570-1577) */
    int divisor;
    if (value < avg)
        divisor = (int)(rx->bit_sample_count / 4);
    else
        divisor = (int)(rx->bit_sample_count * 48);
    return decayavg(avg, value, divisor);
}

static double envelope_decay(const fnavtex_rx *rx, double avg, double value)
{
    /* envelope average decays fast up, slow down (navtex.cxx:1580-1587) */
    int divisor;
    if (value > avg)
        divisor = (int)(rx->bit_sample_count / 4);
    else
        divisor = (int)(rx->bit_sample_count * 16);
    return decayavg(avg, value, divisor);
}

static void process_fft_output(fnavtex_rx *rx, const fdsp_cmpx *zp_mark,
                               const fdsp_cmpx *zp_space, int samples)
{
    /* process_fft_output, navtex.cxx:1589-1670 */
    int i;
    for (i = 0; i < samples; i++) {
        double mark_abs = hypot(zp_mark[i].re, zp_mark[i].im);
        double space_abs = hypot(zp_space[i].re, zp_space[i].im);
        double noise_floor, logic_level, a, me, s, se;

        process_multicorrelator(rx);

        /* noise floor and envelope for mark and space */
        rx->mark_env = envelope_decay(rx, rx->mark_env, mark_abs);
        rx->mark_noise = noise_decay(rx, rx->mark_noise, mark_abs);
        rx->space_env = envelope_decay(rx, rx->space_env, space_abs);
        rx->space_noise = noise_decay(rx, rx->space_noise, space_abs);

        noise_floor = (rx->space_noise + rx->mark_noise) / 2;

        /* clip mark and space to envelope and floor */
        mark_abs = fmin(mark_abs, rx->mark_env);
        mark_abs = fmax(mark_abs, noise_floor);
        space_abs = fmin(space_abs, rx->space_env);
        space_abs = fmax(space_abs, noise_floor);

        /* mark-space discriminator with W7AY's automatic threshold correction
         * (http://www.w7ay.net/site/Technical/ATC/):
         *   (ma - nf)(me - nf) - (sa - nf)(se - nf) - 0.5 ((me - nf)^2 - (se - nf)^2)
         * with the three multiply-adds fldigi's reference build fuses */
        a = mark_abs - noise_floor;
        me = rx->mark_env - noise_floor;
        s = space_abs - noise_floor;
        se = rx->space_env - noise_floor;
        logic_level = fma(fma(me, me, -(se * se)), -0.5, fma(a, me, -(se * s)));

        /* the average magnitude of the logic level at the sample point and a fifth of a bit
         * before and after shows the best time to sample without (noisy) null crossings */
        if ((double)rx->sample_count >= rx->next_early_event) {
            rx->average_early_signal = decayavg(rx->average_early_signal, fabs(logic_level), 64);
            rx->next_early_event += rx->bit_sample_count;
        }
        if ((double)rx->sample_count >= rx->next_late_event) {
            rx->average_late_signal = decayavg(rx->average_late_signal, fabs(logic_level), 64);
            rx->next_late_event += rx->bit_sample_count;
        }
        /* the peak of a signal pulse */
        rx->pulse_edge_event = (double)rx->sample_count >= rx->next_prompt_event;
        if (rx->pulse_edge_event) {
            rx->average_prompt_signal =
                decayavg(rx->average_prompt_signal, fabs(logic_level), 64);
            rx->next_prompt_event += rx->bit_sample_count;
        }

        switch (rx->state) {
        case FNAVTEX_STATE_SYNC_SETUP:
            rx->error_count = 0;
            rx->shift = 0;
            set_state(rx, FNAVTEX_STATE_SYNC);
            break;
        case FNAVTEX_STATE_SYNC:
        case FNAVTEX_STATE_READ_DATA:
            if (rx->pulse_edge_event)
                fnavtex_i_handle_bit_value(rx, to_int(logic_level));
            break;
        default:
            break;
        }
        rx->sample_count++;
    }
    compute_metric(rx);
}

static fdsp_cmpx mixer(const fnavtex_rx *rx, double *phase, double f, fdsp_cmpx in)
{
    /* mixer, navtex.cxx:1559-1567: cmplx(cos(phase), sin(phase)) * in, the product as
     * std::complex evaluates it (four products in separate statements) */
    fdsp_cmpx z;
    double s, c, ac, bd, ad, bc;
    fdsp_sincos(*phase, &s, &c);
    ac = c * in.re;
    bd = s * in.im;
    ad = c * in.im;
    bc = s * in.re;
    z.re = ac - bd;
    z.im = ad + bc;
    *phase -= NAVTEX_TWOPI * f / rx->sample_rate;
    if (*phase < -NAVTEX_TWOPI)
        *phase += NAVTEX_TWOPI;
    return z;
}

static void process_data(fnavtex_rx *rx, const double *data, int nb_samples)
{
    /* process_data, navtex.cxx:1523-1556; the power spectrum fldigi's waterfall keeps
     * current is updated first, from the samples up to this block */
    int i;
    wf_update(rx);
    process_afc(rx);
    fnavtex_i_process_timeout(rx);
    for (i = 0; i < nb_samples; i++) {
        const fdsp_cmpx *zp_mark = NULL, *zp_space = NULL;
        fdsp_cmpx z, zmark, zspace;
        int n_out;
        double dv;

        rx->time_sec = (double)(rx->sample_count / rx->sample_rate);

        dv = 32767 * data[i];
        z.re = dv;
        z.im = dv;

        zmark = mixer(rx, &rx->mark_phase, rx->mark_f, z);
        fdsp_fftfilt_run(&rx->mark_lowpass, zmark, &zp_mark);

        zspace = mixer(rx, &rx->space_phase, rx->space_f, z);
        n_out = fdsp_fftfilt_run(&rx->space_lowpass, zspace, &zp_space);

        rx->wf_ring[rx->wf_pos] = data[i];
        rx->wf_pos = (rx->wf_pos + 1) % NAVTEX_WF_FFTLEN;

        if (n_out)
            process_fft_output(rx, zp_mark, zp_space, n_out);
    }
}

/* ---------------------------------------------------------------------------------- */
/* public API                                                                         */
/* ---------------------------------------------------------------------------------- */

static int configure_filters(fnavtex_rx *rx)
{
    /* configure_filters, navtex.cxx:994-1006: new fftfilt(baud / rate, 512) followed by
     * rtty_filter(baud / rate) for mark and space */
    double fc = rx->baud_rate / rx->sample_rate;
    if (fdsp_fftfilt_init(&rx->mark_lowpass, 0, fc, NAVTEX_FILTLEN) != 0)
        return -1;
    fdsp_fftfilt_rtty_filter(&rx->mark_lowpass, fc);
    if (fdsp_fftfilt_init(&rx->space_lowpass, 0, fc, NAVTEX_FILTLEN) != 0)
        return -1;
    fdsp_fftfilt_rtty_filter(&rx->space_lowpass, fc);
    return 0;
}

static void rx_init_state(fnavtex_rx *rx)
{
    /* navtex_implementation::navtex_implementation, navtex.cxx:942-981, then navtex::set_freq
     * with the configured carrier as fldigi's modem start does */
    double bit_duration_seconds;
    rx->metric = 0.0;
    rx->snr = 0.0;
    rx->time_sec = 0.0;
    rx->state = FNAVTEX_STATE_SYNC_SETUP;
    rx->message_time = 0.0;
    rx->sample_rate = FNAVTEX_RATE;
    rx->shift = 0;
    rx->alpha_phase = 0;
    rx->header_found = 0;
    rx->center_f = FNAVTEX_DEFAULT_CARRIER_HZ;
    rx->baud_rate = NAVTEX_BAUD;
    bit_duration_seconds = 1.0 / rx->baud_rate;
    rx->bit_sample_count = rx->sample_rate * bit_duration_seconds;
    rx->next_early_event = 0;
    rx->next_prompt_event = rx->bit_sample_count / 5;
    rx->next_late_event = rx->bit_sample_count * 2 / 5;
    rx->average_early_signal = 0;
    rx->average_prompt_signal = 0;
    rx->average_late_signal = 0;
    rx->error_count = 0;
    rx->sample_count = 0;
    memset(rx->bit_values, 0, sizeof rx->bit_values);
    rx->bit_cursor = 0;
    rx->pulse_edge_event = 0;
    rx->char_from_fec = 0;
    rx->mark_env = rx->space_env = rx->mark_noise = rx->space_noise = 0;
    rx->last_char = 0;
    rx->sigpwr = rx->noisepwr = 0.0;
    rx->cnt_upd = 0;
    rx->cnt_read_data = 0;
    rx->block_n = 0;
    rx->wf_pos = 0;
    memset(rx->wf_ring, 0, NAVTEX_WF_FFTLEN * sizeof(double));
    memset(rx->pwr, 0, (NAVTEX_WF_WIDTH + 1) * sizeof(double));
    rx->wf_carrier = (int)FNAVTEX_DEFAULT_CARRIER_HZ;
    rx->q_head = rx->q_count = 0;
    rx->tap_head = rx->tap_count = 0;
    rx->dropped = 0;
    msg_reset(rx);
    set_filter_values(rx);
    set_freq(rx, rx->cfg_carrier);
}

static int carrier_ok(double f)
{
    return f >= 200.0 && f <= 3000.0;
}

fnavtex_rx *fnavtex_rx_create(const fnavtex_rx_config *cfg)
{
    fnavtex_rx *rx;
    double carrier;
    int i;
    double pwr = 0.0, inv_n;
    if (!cfg || (cfg->mode != FNAVTEX_MODE_NAVTEX && cfg->mode != FNAVTEX_MODE_SITORB))
        return NULL;
    carrier = cfg->carrier_hz == 0 ? FNAVTEX_DEFAULT_CARRIER_HZ : cfg->carrier_hz;
    if (!carrier_ok(carrier) || cfg->min_msg_len < 0)
        return NULL;
    rx = (fnavtex_rx *)calloc(1, sizeof *rx);
    if (!rx)
        return NULL;
    rx->only_sitor_b = cfg->mode == FNAVTEX_MODE_SITORB;
    rx->ita2 = cfg->ita2 != 0;
    rx->afc = cfg->afc != 0;
    rx->min_msg_len = cfg->min_msg_len;
    rx->cfg_carrier = carrier;
    rx->baud_rate = NAVTEX_BAUD;
    rx->sample_rate = FNAVTEX_RATE;
    rx->msg = (char *)malloc(NAVTEX_MSG_MAX + 1);
    rx->scratch = (char *)malloc(NAVTEX_MSG_MAX + 1);
    rx->wf_ring = (double *)calloc(NAVTEX_WF_FFTLEN, sizeof(double));
    rx->wf_window = (double *)malloc(NAVTEX_WF_FFTLEN * sizeof(double));
    rx->wf_buf = (fdsp_cmpx *)malloc(NAVTEX_WF_FFTLEN * sizeof(fdsp_cmpx));
    rx->pwr = (double *)calloc(NAVTEX_WF_WIDTH + 1, sizeof(double));
    for (i = 0; i < NAVTEX_QUEUE; i++)
        rx->queue[i].text = (char *)malloc(NAVTEX_MSG_MAX + NAVTEX_DECOR_MAX + 1);
    if (!rx->msg || !rx->scratch || !rx->wf_ring || !rx->wf_window || !rx->wf_buf ||
        !rx->pwr || configure_filters(rx) != 0 ||
        fdsp_gfft_init(&rx->wfft, NAVTEX_WF_FFTLEN) != 0) {
        fnavtex_rx_destroy(rx);
        return NULL;
    }
    for (i = 0; i < NAVTEX_QUEUE; i++) {
        if (!rx->queue[i].text) {
            fnavtex_rx_destroy(rx);
            return NULL;
        }
    }
    /* BlackmanWindow, misc.h:128-138 */
    inv_n = 1.0 / (double)NAVTEX_WF_FFTLEN;
    for (i = 0; i < NAVTEX_WF_FFTLEN; i++) {
        rx->wf_window[i] = fdsp_blackman((double)i * inv_n);
        pwr += rx->wf_window[i] * rx->wf_window[i];
    }
    pwr = sqrt((double)NAVTEX_WF_FFTLEN / pwr);
    for (i = 0; i < NAVTEX_WF_FFTLEN; i++)
        rx->wf_window[i] *= pwr;
    rx_init_state(rx);
    rx->footprint = sizeof *rx + 2 * (NAVTEX_MSG_MAX + 1) +
                    NAVTEX_WF_FFTLEN * (2 * sizeof(double) + sizeof(fdsp_cmpx)) +
                    (NAVTEX_WF_WIDTH + 1) * sizeof(double) +
                    NAVTEX_QUEUE * (NAVTEX_MSG_MAX + NAVTEX_DECOR_MAX + 1) +
                    2 * NAVTEX_FILTLEN * 5 * sizeof(fdsp_cmpx) +
                    (NAVTEX_WF_FFTLEN / 4 + 1) * sizeof(double) + NAVTEX_WF_FFTLEN * sizeof(short);
    return rx;
}

void fnavtex_rx_destroy(fnavtex_rx *rx)
{
    int i;
    if (!rx)
        return;
    fdsp_fftfilt_free(&rx->mark_lowpass);
    fdsp_fftfilt_free(&rx->space_lowpass);
    fdsp_gfft_free(&rx->wfft);
    free(rx->msg);
    free(rx->scratch);
    free(rx->wf_ring);
    free(rx->wf_window);
    free(rx->wf_buf);
    free(rx->pwr);
    for (i = 0; i < NAVTEX_QUEUE; i++)
        free(rx->queue[i].text);
    free(rx);
}

void fnavtex_rx_reset(fnavtex_rx *rx)
{
    double fc;
    if (!rx)
        return;
    fc = rx->baud_rate / rx->sample_rate;
    fdsp_fftfilt_create(&rx->mark_lowpass, 0, fc);
    fdsp_fftfilt_rtty_filter(&rx->mark_lowpass, fc);
    fdsp_fftfilt_create(&rx->space_lowpass, 0, fc);
    fdsp_fftfilt_rtty_filter(&rx->space_lowpass, fc);
    rx_init_state(rx);
}

static size_t run_blocks(fnavtex_rx *rx, fnavtex_event *ev, size_t max_ev, const double *x,
                         size_t n, int flush)
{
    size_t off = 0;
    rx->ev = ev;
    rx->ev_max = ev ? max_ev : 0;
    rx->ev_n = 0;
    while (off < n) {
        size_t take = (size_t)(FNAVTEX_BLOCK - rx->block_n);
        if (take > n - off)
            take = n - off;
        memcpy(rx->block + rx->block_n, x + off, take * sizeof(double));
        rx->block_n += (int)take;
        off += take;
        if (rx->block_n == FNAVTEX_BLOCK) {
            process_data(rx, rx->block, FNAVTEX_BLOCK);
            rx->block_n = 0;
        }
    }
    if (flush && rx->block_n > 0) {
        process_data(rx, rx->block, rx->block_n);
        rx->block_n = 0;
    }
    rx->ev = NULL;
    return rx->ev_n;
}

size_t fnavtex_rx_process(fnavtex_rx *rx, const double *x, size_t n, fnavtex_event *ev,
                          size_t max_ev)
{
    if (!rx || (!x && n))
        return 0;
    return run_blocks(rx, ev, max_ev, x, n, 0);
}

size_t fnavtex_rx_flush(fnavtex_rx *rx, fnavtex_event *ev, size_t max_ev)
{
    if (!rx)
        return 0;
    return run_blocks(rx, ev, max_ev, NULL, 0, 1);
}

size_t fnavtex_rx_pending(const fnavtex_rx *rx)
{
    return rx ? rx->q_count : 0;
}

int fnavtex_rx_take_message(fnavtex_rx *rx, fnavtex_message *m, char *text, size_t max)
{
    navtex_qmsg *q;
    if (!rx || rx->q_count == 0)
        return 0;
    q = &rx->queue[rx->q_head];
    if (m)
        *m = q->m;
    if (text && max > 0) {
        size_t k = q->m.text_len < max - 1 ? q->m.text_len : max - 1;
        memcpy(text, q->text, k);
        text[k] = '\0';
    }
    rx->q_head = (rx->q_head + 1) % NAVTEX_QUEUE;
    rx->q_count--;
    return 1;
}

double fnavtex_rx_snr_db(const fnavtex_rx *rx)
{
    return rx ? rx->snr : 0.0;
}

double fnavtex_rx_metric(const fnavtex_rx *rx)
{
    return rx ? rx->metric : 0.0;
}

double fnavtex_rx_carrier_hz(const fnavtex_rx *rx)
{
    return rx ? rx->center_f : 0.0;
}

void fnavtex_rx_set_carrier(fnavtex_rx *rx, double carrier_hz)
{
    if (!rx || !carrier_ok(carrier_hz))
        return;
    set_freq(rx, carrier_hz);
}

int fnavtex_rx_state(const fnavtex_rx *rx)
{
    return rx ? rx->state : FNAVTEX_STATE_SYNC_SETUP;
}

long long fnavtex_rx_samples(const fnavtex_rx *rx)
{
    return rx ? (long long)rx->sample_count : 0;
}

size_t fnavtex_rx_dropped(const fnavtex_rx *rx)
{
    return rx ? rx->dropped : 0;
}

size_t fnavtex_rx_footprint(const fnavtex_rx *rx)
{
    return rx ? rx->footprint : 0;
}

size_t fnavtex_rx_take_bits(fnavtex_rx *rx, long long *samples, int *levels, size_t max)
{
    size_t k = 0;
    if (!rx)
        return 0;
    while (rx->tap_count > 0 && k < max) {
        if (samples)
            samples[k] = rx->tap_samples[rx->tap_head];
        if (levels)
            levels[k] = rx->tap_levels[rx->tap_head];
        rx->tap_head = (rx->tap_head + 1) % NAVTEX_BIT_TAP;
        rx->tap_count--;
        k++;
    }
    return k;
}
