/*
 * fldigi-navtex-core -- the NAVTEX and SITOR-B modem of fldigi 4.2.13 (CCIR 476 collective
 * B-mode FEC, 100 baud, 170 Hz shift), receiver and transmitter, ported to C99.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain
 * Ported from fldigi 4.2.13 (https://github.com/wave-owl/fldigi, tag v4.2.13, commit
 * e7c3ab3709ad62a7d91829364eb48a05fa325c0d): src/navtex/navtex.cxx and src/include/navtex.h,
 * (c) 2011-2016 Remi Chateauneu F4ECW and Rik van Riel AB1KW, adapted from JNX (c) Paul Lutus;
 * the station power spectrum of src/waterfall/waterfall.cxx (c) Dave Freese W1HKJ.
 *
 * This program is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this
 * program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef FLDIGI_NAVTEX_H
#define FLDIGI_NAVTEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FNAVTEX_VERSION_STRING "0.1.1"
#define FNAVTEX_ABI 1

/* "fldigi-navtex-core 0.1.1 contract=navtex-c1 abi=1 tx=1 dsp=0.7.0". Static string. The
 * contract tag changes only with an incompatible API change; tx=1 announces the
 * transmitter. */
const char *fnavtex_version(void);
int fnavtex_abi(void);                      /* FNAVTEX_ABI */

/* sizeof the public structures, so a binding can check its mirrors of them (a ctypes
 * binding sizes its arrays from these). */
#define FNAVTEX_SIZEOF_EVENT 0
#define FNAVTEX_SIZEOF_MESSAGE 1
#define FNAVTEX_SIZEOF_RX_CONFIG 2
#define FNAVTEX_SIZEOF_TX_CONFIG 3
size_t fnavtex_sizeof(int what);            /* 0 for an unknown structure */

/* The two modes of fldigi's navtex modem (navtex.cxx:1940-1957). NAVTEX frames messages as
 * "ZCZC B1B2B3B4" .. "NNNN" with phasing; SITOR-B is the plain collective B-mode stream. */
#define FNAVTEX_MODE_NAVTEX 0
#define FNAVTEX_MODE_SITORB 1

#define FNAVTEX_RATE 11025                  /* modem::samplerate, navtex.cxx:1944 */
#define FNAVTEX_SHIFT_HZ 85                 /* deviation_f, navtex.cxx:860: mark/space +-85 Hz */
#define FNAVTEX_BANDWIDTH_HZ 170            /* modem::bandwidth = 2 * deviation_f, :1945 */
#define FNAVTEX_DEFAULT_CARRIER_HZ 1000.0   /* dflt_center_freq, navtex.cxx:862 */
#define FNAVTEX_BLOCK 512                   /* sound-card block the receiver is driven with */

/* ------------------------------------------------------------------------------------
 * Receiver: navtex_implementation::process_data (navtex.cxx:1523-1556) and everything below
 * it: mark/space mixers, fldigi's raised-cosine RTTY low passes (fftfilt, 512 points), W7AY's
 * envelope/noise discriminator, the early/prompt/late multicorrelator bit sync, the
 * alpha/rep phase search, the CCIR 476 FEC cascade and the ZCZC/NNNN message framing with
 * the 600 s timeout. The audio is processed in fldigi's sound-card blocks of FNAVTEX_BLOCK
 * samples (the AFC and the timeout run once per block, as in fldigi), so the output does
 * not depend on how the caller chunks the input.
 * ------------------------------------------------------------------------------------ */
typedef struct fnavtex_rx_config {
    int mode;               /* FNAVTEX_MODE_NAVTEX or FNAVTEX_MODE_SITORB */
    double carrier_hz;      /* audio centre between mark and space, 0 = 1000 Hz; 200 .. 3000 */
    int afc;                /* progStatus.afconoff (off in the fldigi bench) */
    int ita2;               /* progdefaults.ITA2: 1 = ITA2 figures, 0 = US-TTY (fldigi's
                             * default) */
    int min_msg_len;        /* progdefaults.NVTX_MinSizLoggedMsg (fldigi's default 0) */
} fnavtex_rx_config;

/* One character fldigi prints (filter_print, navtex.cxx:1364-1371: BEL shows as an
 * apostrophe, CR is not printed). */
#define FNAVTEX_EV_FEC 0x01     /* the character came from the FEC cascade (its rep, the
                                 * alpha/rep average or a flipped bit), not the alpha copy */
typedef struct fnavtex_event {
    unsigned char ch;
    unsigned char flags;
} fnavtex_event;

/* One message fldigi logs (display_message, navtex.cxx:1683-1698). */
#define FNAVTEX_MSG_HEADER 0x01     /* a "ZCZC B1B2B3B4" header was seen (else fldigi prefixes
                                     * "[Lost header]:") */
#define FNAVTEX_MSG_END 0x02        /* ended by "NNNN" (detect_end) */
#define FNAVTEX_MSG_NEW_HEADER 0x04 /* cut by the next header (":[Lost trailer]") */
#define FNAVTEX_MSG_TIMEOUT 0x08    /* flushed after 600 s (":<TIMEOUT>") */
typedef struct fnavtex_message {
    char origin;            /* B1, '?' without a header */
    char subject;           /* B2, '?' without a header */
    int number;             /* B3B4 0 .. 99, 0 without a header */
    int flags;              /* FNAVTEX_MSG_* */
    size_t body_len;        /* length of fldigi's message string (the size compared with
                             * min_msg_len) */
    size_t text_len;        /* length of the logged text, i.e. what fldigi's XML-RPC
                             * navtex.get_message returns ("[Lost header]:" .. ":[Lost
                             * trailer]" decorations included) */
    unsigned fec_chars;     /* characters of the message taken from the FEC cascade */
    unsigned failed_chars;  /* characters the FEC cascade could not recover */
    long long end_sample;   /* receiver sample count when the message was logged */
} fnavtex_message;

/* Receiver states (navtex.cxx:869-871). */
#define FNAVTEX_STATE_SYNC_SETUP 0
#define FNAVTEX_STATE_SYNC 1
#define FNAVTEX_STATE_READ_DATA 2

typedef struct fnavtex_rx fnavtex_rx;

/* NULL for a bad mode or carrier, or out of memory. Everything is allocated here. */
fnavtex_rx *fnavtex_rx_create(const fnavtex_rx_config *cfg);
void fnavtex_rx_destroy(fnavtex_rx *rx);
/* Back to the state after create (the carrier of the config, pending messages dropped). */
void fnavtex_rx_reset(fnavtex_rx *rx);
/* Feed n samples at 11025 Hz on fldigi's sound-card scale (int16 / 32768). Complete blocks
 * of FNAVTEX_BLOCK samples are processed, the rest waits for the next call. One event per
 * printed character goes to ev; returns their number. Events beyond max_ev are dropped and
 * counted (fnavtex_rx_dropped). ev may be NULL with max_ev 0. */
size_t fnavtex_rx_process(fnavtex_rx *rx, const double *x, size_t n, fnavtex_event *ev,
                          size_t max_ev);
/* Process the waiting partial block now, as fldigi processes the short last block of a
 * file (end of input). */
size_t fnavtex_rx_flush(fnavtex_rx *rx, fnavtex_event *ev, size_t max_ev);
/* Logged messages waiting to be taken (at most 16 are kept; older ones are dropped and
 * counted). */
size_t fnavtex_rx_pending(const fnavtex_rx *rx);
/* Take the oldest message: fills *m and copies its text (NUL-terminated, cut to max - 1
 * bytes; m->text_len is the full length). Returns 1, or 0 when none is waiting. text may be
 * NULL with max 0. */
int fnavtex_rx_take_message(fnavtex_rx *rx, fnavtex_message *m, char *text, size_t max);
/* fldigi's s/n in dB and the metric 0 .. 100 (compute_metric, navtex.cxx:1373-1393), from
 * the core's power spectrum in place of fldigi's waterfall. */
double fnavtex_rx_snr_db(const fnavtex_rx *rx);
double fnavtex_rx_metric(const fnavtex_rx *rx);
/* Current centre frequency (moves with the AFC). */
double fnavtex_rx_carrier_hz(const fnavtex_rx *rx);
/* navtex::set_freq (navtex.cxx:1995-1999): retune, mixer phases restart at 0. */
void fnavtex_rx_set_carrier(fnavtex_rx *rx, double carrier_hz);
int fnavtex_rx_state(const fnavtex_rx *rx);         /* FNAVTEX_STATE_* */
long long fnavtex_rx_samples(const fnavtex_rx *rx); /* samples processed (64-bit) */
size_t fnavtex_rx_dropped(const fnavtex_rx *rx);    /* events and messages dropped */
size_t fnavtex_rx_footprint(const fnavtex_rx *rx);  /* bytes allocated (all at create) */
/* Test tap: the logic levels handed to handle_bit_value since the last call (oldest first,
 * at most the last 4096) with the sample count of each. Returns the number copied. */
size_t fnavtex_rx_take_bits(fnavtex_rx *rx, long long *samples, int *levels, size_t max);

/* ccir_message::cleanup (navtex.cxx:663-690): CR/LF runs become one "\n", blank runs one
 * space, leading white space is dropped (what fldigi writes to its ADIF/KML logs). Returns
 * the output length; out gets at most max - 1 bytes and a NUL. */
size_t fnavtex_cleanup(const char *in, size_t n, char *out, size_t max);
/* ccir_message::msg_type (navtex.cxx:637-660): the English name of subject indicator B2. */
const char *fnavtex_subject_name(char subject);

/* ------------------------------------------------------------------------------------
 * Transmitter: navtex_implementation::send_message (navtex.cxx:1822-1854). Output: the
 * samples fldigi hands to modem::ModulateXmtr, 0.9 * sin of one phase accumulator shared by
 * phasing and bits (send_sine, :1781-1792), before the TX level (multiply by 10^(txlevel/20),
 * fldigi's -12 dB, to reproduce fldigi's WAV). Each bit is 110 samples (fldigi truncates
 * 0.01 s * 11025), mark (1) = carrier + 85 Hz.
 *
 * NAVTEX mode frames every message as fldigi does: phasing (10 s for the first message of
 * a transmission, 5 s otherwise), "ZCZC ZI<nn>\r\n" (origin Z, subject I, a counter that
 * starts at 1), the text, "\r\nNNNN\r\n\n" and, after the last message, 2 s of phasing.
 * SITOR-B mode sends the text alone. Both: letters/figures shifts (encode, :1752-1760) and
 * the B-mode interleave of every character with its repetition five characters later,
 * between two rep/alpha pairs and two char32 tails (create_fec, :1728-1749).
 * ------------------------------------------------------------------------------------ */
#define FNAVTEX_TX_MAX_TEXT 4096    /* bytes of one message */

typedef struct fnavtex_tx_config {
    int mode;               /* FNAVTEX_MODE_NAVTEX or FNAVTEX_MODE_SITORB */
    double carrier_hz;      /* 0 = 1000 Hz; 200 .. 3000 */
    int ita2;               /* figures table as for the receiver (0 = US-TTY) */
    int first_number;       /* NAVTEX header number of the first message, 0 .. 99;
                             * -1 = fldigi's 1 (m_message_counter, navtex.cxx:946) */
} fnavtex_tx_config;

typedef struct fnavtex_tx fnavtex_tx;

fnavtex_tx *fnavtex_tx_create(const fnavtex_tx_config *cfg);
void fnavtex_tx_destroy(fnavtex_tx *tx);
/* Load one message (bytes outside 1 .. 127 and bytes without a CCIR 476 code are skipped as
 * fldigi skips characters it cannot encode). Returns the samples the message takes, or 0
 * (and nothing loaded) when the text is longer than FNAVTEX_TX_MAX_TEXT or, in SITOR-B mode,
 * encodes to fewer than two codes (fldigi's create_fec reads before its buffer then).
 * Replaces a message not fully rendered. is_first/is_last select the phasing as fldigi's
 * transmit_message_async does for a queue of messages. */
size_t fnavtex_tx_load(fnavtex_tx *tx, const char *text, size_t n, int is_first,
                       int is_last);
/* The next samples of the loaded message; returns the number written, 0 when it is done. */
size_t fnavtex_tx_render(fnavtex_tx *tx, double *out, size_t max);
size_t fnavtex_tx_remaining(const fnavtex_tx *tx);
/* Header number the next NAVTEX message gets (0 .. 99). */
int fnavtex_tx_next_number(const fnavtex_tx *tx);
/* Test tap: the 7-bit B-mode sequence of the loaded message (create_fec output). Returns its
 * length; copies at most max codes. */
size_t fnavtex_tx_codes(const fnavtex_tx *tx, unsigned char *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif
