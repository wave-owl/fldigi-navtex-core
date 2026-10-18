/*
 * fldigi-navtex-core: the NAVTEX / SITOR-B transmitter of fldigi 4.2.13
 * (src/navtex/navtex.cxx, navtex_implementation::send_message and below).
 *
 * Copyright (C) 2026 Gerolf Ziegenhain
 * Original C++: (c) 2011-2016 Remi Chateauneu F4ECW, Rik van Riel AB1KW; JNX (c) Paul Lutus.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * fldigi renders a whole message in one tx_process() call; here the message is loaded and
 * then rendered in pieces of any size with the same samples. send_sine's phase is a
 * function-static variable in fldigi (shared by every message and modem instance); here it
 * belongs to the transmitter and starts at 0 at create.
 */
#include "navtex_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A NAVTEX message adds "ZCZC ZI00\r\n" and "\r\nNNNN\r\n\n" (19 bytes, up to 4 shifts). */
#define TX_FRAME_EXTRA 32
#define TX_MAX_CODES (2 * (FNAVTEX_TX_MAX_TEXT + TX_FRAME_EXTRA))
#define TX_MAX_FEC (2 * TX_MAX_CODES + 8)

fnavtex_tx *fnavtex_tx_create(const fnavtex_tx_config *cfg)
{
    fnavtex_tx *tx;
    double carrier;
    if (!cfg || (cfg->mode != FNAVTEX_MODE_NAVTEX && cfg->mode != FNAVTEX_MODE_SITORB))
        return NULL;
    carrier = cfg->carrier_hz == 0 ? FNAVTEX_DEFAULT_CARRIER_HZ : cfg->carrier_hz;
    if (!(carrier >= 200.0 && carrier <= 3000.0))
        return NULL;
    if (cfg->first_number < -1 || cfg->first_number > 99)
        return NULL;
    tx = (fnavtex_tx *)calloc(1, sizeof *tx);
    if (!tx)
        return NULL;
    tx->codes = (unsigned char *)malloc(TX_MAX_CODES);
    tx->fec = (unsigned char *)malloc(TX_MAX_FEC);
    tx->frame = (char *)malloc(FNAVTEX_TX_MAX_TEXT + TX_FRAME_EXTRA);
    if (!tx->codes || !tx->fec || !tx->frame) {
        fnavtex_tx_destroy(tx);
        return NULL;
    }
    tx->only_sitor_b = cfg->mode == FNAVTEX_MODE_SITORB;
    tx->ita2 = cfg->ita2 != 0;
    /* navtex_implementation ctor (navtex.cxx:942-981) and set_carrier (:1905-1909) */
    tx->message_counter = cfg->first_number < 0 ? 1 : cfg->first_number;
    tx->baud_rate = NAVTEX_BAUD;
    tx->sample_rate = FNAVTEX_RATE;
    tx->center_f = carrier;
    tx->mark_f = tx->center_f + FNAVTEX_SHIFT_HZ;
    tx->space_f = tx->center_f - FNAVTEX_SHIFT_HZ;
    tx->phase = 0;
    fnavtex_encoder_tables(tx->ita2, tx->ltrs_to_code, tx->figs_to_code);
    /* send_bit -> send_sine(1.0 / baud): nb_samples = seconds * samplerate as int
     * (navtex.cxx:1784, 1799-1802), 110 for 0.01 s at 11025 Hz */
    tx->bit_samples = (size_t)(int)(1.0 / (double)tx->baud_rate * tx->sample_rate);
    return tx;
}

void fnavtex_tx_destroy(fnavtex_tx *tx)
{
    if (!tx)
        return;
    free(tx->codes);
    free(tx->fec);
    free(tx->frame);
    free(tx);
}

static size_t phasing_samples(const fnavtex_tx *tx, double seconds)
{
    /* send_phasing(int seconds) -> send_sine(seconds, carrier) (navtex.cxx:1794-1797) */
    return (size_t)(int)(seconds * tx->sample_rate);
}

size_t fnavtex_tx_load(fnavtex_tx *tx, const char *text, size_t n, int is_first,
                       int is_last)
{
    /* send_message, navtex.cxx:1822-1854, and send_string, :1804-1821 */
    size_t ncodes;
    if (!tx || (!text && n) || n > FNAVTEX_TX_MAX_TEXT)
        return 0;
    if (tx->only_sitor_b) {
        ncodes = fnavtex_encode(tx->ltrs_to_code, tx->figs_to_code, text, n, tx->codes);
        if (ncodes < 2)
            return 0;
        tx->pre_samples = 0;
        tx->post_samples = 0;
    } else {
        /* phasing, the header with origin Z (never seen) and subject I (not used), the text,
         * "\r\nNNNN\r\n\n" (the extra CR LF before NNNN "clarifies things", fldigi) */
        char preamble[64];
        char *full = tx->frame;
        size_t lp, k;
        const char origin = 'Z';
        const char subject = 'I';
        static const char tail[] = "\r\nNNNN\r\n\n";
        preamble[0] = 'Z';
        preamble[1] = 'C';
        preamble[2] = 'Z';
        preamble[3] = 'C';
        preamble[4] = ' ';
        preamble[5] = origin;
        preamble[6] = subject;
        preamble[7] = (char)('0' + tx->message_counter / 10);
        preamble[8] = (char)('0' + tx->message_counter % 10);
        preamble[9] = '\r';
        preamble[10] = '\n';
        lp = 11;
        tx->message_counter = (tx->message_counter + 1) % 100;
        memcpy(full, preamble, lp);
        if (n)
            memcpy(full + lp, text, n);
        memcpy(full + lp + n, tail, sizeof tail - 1);
        k = lp + n + sizeof tail - 1;
        ncodes = fnavtex_encode(tx->ltrs_to_code, tx->figs_to_code, full, k, tx->codes);
        tx->pre_samples = phasing_samples(tx, is_first ? 10.0 : 5.0);
        tx->post_samples = is_last ? phasing_samples(tx, 2.0) : 0;
    }
    tx->n_fec = fnavtex_create_fec(tx->codes, ncodes, tx->fec);
    tx->total = tx->pre_samples + tx->n_fec * 7 * tx->bit_samples + tx->post_samples;
    tx->pos = 0;
    return tx->total;
}

static double send_sine_sample(fnavtex_tx *tx, double freq)
{
    /* one sample of send_sine, navtex.cxx:1781-1792 */
    const double max_level = 0.9;
    double ratio = 2.0 * M_PI * (double)freq / (double)tx->sample_rate;
    double v = max_level * sin(tx->phase += ratio);
    if (tx->phase > 2.0 * M_PI)
        tx->phase -= 2.0 * M_PI;
    return v;
}

size_t fnavtex_tx_render(fnavtex_tx *tx, double *out, size_t max)
{
    size_t k = 0;
    size_t bits_end;
    if (!tx || !out)
        return 0;
    bits_end = tx->pre_samples + tx->n_fec * 7 * tx->bit_samples;
    while (k < max && tx->pos < tx->total) {
        double f;
        if (tx->pos < tx->pre_samples || tx->pos >= bits_end) {
            f = tx->center_f;
        } else {
            /* send_string: seven bits per code, least significant first, 1 = mark */
            size_t b = (tx->pos - tx->pre_samples) / tx->bit_samples;
            unsigned char c = tx->fec[b / 7];
            int bit = (c >> (b % 7)) & 1;
            f = bit ? tx->mark_f : tx->space_f;
        }
        out[k++] = send_sine_sample(tx, f);
        tx->pos++;
    }
    return k;
}

size_t fnavtex_tx_remaining(const fnavtex_tx *tx)
{
    return tx ? tx->total - tx->pos : 0;
}

int fnavtex_tx_next_number(const fnavtex_tx *tx)
{
    return tx ? tx->message_counter : 0;
}

size_t fnavtex_tx_codes(const fnavtex_tx *tx, unsigned char *out, size_t max)
{
    if (!tx)
        return 0;
    if (out)
        memcpy(out, tx->fec, tx->n_fec < max ? tx->n_fec : max);
    return tx->n_fec;
}
