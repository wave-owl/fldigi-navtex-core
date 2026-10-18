/*
 * fldigi-navtex-core internals: the CCIR 476 tables, the receiver and transmitter state
 * (fldigi 4.2.13 src/navtex/navtex.cxx). The C tests include this header to drive the FEC
 * cascade and the framing directly.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NAVTEX_INTERNAL_H
#define NAVTEX_INTERNAL_H

#include "fldigi_navtex.h"
#include "fldigi_dsp.h"

#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define NAVTEX_TWOPI (2.0 * M_PI)          /* TWOPI, modem.h:45 */

/* CCIR 476 service codes, navtex.cxx:502-508 */
#define NAVTEX_CODE_LTRS 0x5a
#define NAVTEX_CODE_FIGS 0x36
#define NAVTEX_CODE_ALPHA 0x0f
#define NAVTEX_CODE_BETA 0x33
#define NAVTEX_CODE_CHAR32 0x6a
#define NAVTEX_CODE_REP 0x66
#define NAVTEX_CHAR_BELL 0x07

#define NAVTEX_BAUD 100                     /* m_baud_rate, navtex.cxx:958 */
#define NAVTEX_FILTLEN 512                  /* filtlen, navtex.cxx:998 */
#define NAVTEX_BITS 100                     /* m_bit_values.resize(m_baud_rate), :973 */
#define NAVTEX_HEADER_LEN 10                /* ccir_message::header_len, navtex.cxx:625 */
#define NAVTEX_TIMEOUT_S 600                /* process_timeout, navtex.cxx:1037 */
/* The message string: fldigi's std::string grows without bound; here a fixed buffer. A
 * NAVTEX message cannot exceed it (at 100 baud and 14 bits per character at most 4286
 * characters arrive before the 600 s timeout flushes); in SITOR-B mode the string is never
 * shown and its oldest half is dropped when it is full. */
#define NAVTEX_MSG_MAX 8192
#define NAVTEX_DECOR_MAX 32                 /* "[Lost header]:" + ":[Lost trailer]" */
#define NAVTEX_QUEUE 16
#define NAVTEX_BIT_TAP 4096

/* Station power spectrum in place of fldigi's waterfall (WFdisp::processFFT,
 * waterfall.cxx:599-656): a Blackman-windowed FFT of the last WF_FFTLEN samples, power per
 * 1 Hz column up to the waterfall width. */
#define NAVTEX_WF_FFTLEN 8192               /* WF_FFTLEN, waterfall.h:45 */
#define NAVTEX_WF_WIDTH 3000                /* IMAGE_WIDTH = HighFreqCutoff 3000 */
/* fldigi's waterfall limits LowFreqCutoff 0 / HighFreqCutoff 3000 (configuration.h:204-209)
 * bound modem::set_freq (modem.cxx:319-324) and so the waterfall cursor. */
#define NAVTEX_LOW_CUTOFF 0
#define NAVTEX_HIGH_CUTOFF 3000

extern const unsigned char fnavtex_code_to_ltrs[128];
extern const unsigned char fnavtex_ustty_code_to_figs[128];
extern const unsigned char fnavtex_ita2_code_to_figs[128];

/* CCIR476::check_bits (navtex.cxx:593-601): exactly four of the seven bits set. */
int fnavtex_check_bits(int v);
/* CCIR476::code_to_char (navtex.cxx:562-575): the character, or -code. */
int fnavtex_code_to_char(int code, int shift, int ita2);
/* CCIR476::bytes_to_code (navtex.cxx:577-584) on seven confidence values. */
int fnavtex_bytes_to_code(const int *pos);
/* CCIR476::valid_char_at (navtex.cxx:604-613). */
int fnavtex_valid_char_at(const int *pos);
/* The two encoder tables of the CCIR476 constructor (navtex.cxx:516-539). */
void fnavtex_encoder_tables(int ita2, unsigned char ltrs_to_code[128],
                            unsigned char figs_to_code[128]);
/* encode (navtex.cxx:1752-1760) with char_to_code (:541-560); returns the number of codes
 * written to out (at most 2 per input byte). */
size_t fnavtex_encode(const unsigned char ltrs_to_code[128], const unsigned char figs_to_code[128],
                      const char *text, size_t n, unsigned char *out);
/* create_fec (navtex.cxx:1728-1749) for n >= 2 codes: 2 * n + 8 codes to out. */
size_t fnavtex_create_fec(const unsigned char *codes, size_t n, unsigned char *out);

typedef struct navtex_qmsg {
    fnavtex_message m;
    char *text;                 /* NAVTEX_MSG_MAX + NAVTEX_DECOR_MAX + 1 bytes */
} navtex_qmsg;

struct fnavtex_rx {
    /* configuration */
    int only_sitor_b;           /* m_only_sitor_b */
    int ita2;
    int afc;
    int min_msg_len;
    double cfg_carrier;

    /* the message accumulator, ccir_message m_curr_msg (navtex.cxx:624-858) */
    char *msg;
    size_t msg_len;
    char origin, subject;
    int number;
    unsigned msg_fec, msg_failed;
    char *scratch;              /* cleanup()/decoration work buffer */

    double metric;
    double snr;
    double message_time;        /* m_message_time */
    double time_sec;            /* m_time_sec */
    double mark_f, space_f;
    double center_f;            /* m_center_frequency_f */
    double baud_rate;
    int sample_rate;
    double bit_sample_count;
    fdsp_fftfilt mark_lowpass, space_lowpass;
    double mark_phase, space_phase;
    int state;
    int64_t sample_count;       /* fldigi: int (wraps after 2.25 days); here 64 bits */
    double next_early_event, next_prompt_event, next_late_event;
    double average_early_signal, average_prompt_signal, average_late_signal;
    int bit_values[NAVTEX_BITS];
    int bit_cursor;
    int shift;
    int pulse_edge_event;
    int error_count;
    int alpha_phase;
    int header_found;
    int char_from_fec;          /* the character being decoded came from the FEC cascade */

    /* fldigi's function-static variables, per instance here */
    double mark_env, space_env, mark_noise, space_noise;   /* process_fft_output */
    int last_char;                                          /* process_char */
    double sigpwr, noisepwr;                                /* compute_metric */
    size_t cnt_upd;                                         /* process_afc */
    int cnt_read_data;

    /* sound-card block */
    double block[FNAVTEX_BLOCK];
    int block_n;

    /* power spectrum */
    fdsp_gfft wfft;
    double *wf_ring;            /* NAVTEX_WF_FFTLEN past samples */
    int wf_pos;
    double *wf_window;
    fdsp_cmpx *wf_buf;
    double *pwr;                /* NAVTEX_WF_WIDTH + 1 columns */
    int wf_carrier;             /* WFdisp::carrierfreq */

    /* outputs */
    fnavtex_event *ev;
    size_t ev_max, ev_n;
    size_t dropped;
    navtex_qmsg queue[NAVTEX_QUEUE];
    size_t q_head, q_count;
    long long tap_samples[NAVTEX_BIT_TAP];
    int tap_levels[NAVTEX_BIT_TAP];
    size_t tap_head, tap_count;
    size_t footprint;
};

/* The FEC cascade and the framing, exposed for the C tests. */
int fnavtex_i_process_bytes(fnavtex_rx *rx, int cursor);
void fnavtex_i_flip_smallest_bit(int *pos);
int fnavtex_i_find_alpha_characters(const fnavtex_rx *rx);
void fnavtex_i_handle_bit_value(fnavtex_rx *rx, int logic_level);
void fnavtex_i_process_char(fnavtex_rx *rx, int chr);
void fnavtex_i_process_timeout(fnavtex_rx *rx);

struct fnavtex_tx {
    int only_sitor_b;
    int ita2;
    double center_f;
    double mark_f, space_f;
    double baud_rate;
    int sample_rate;
    int message_counter;        /* m_message_counter */
    double phase;               /* send_sine's static phase, per instance here */
    unsigned char ltrs_to_code[128], figs_to_code[128];
    unsigned char *codes;       /* encode() output */
    unsigned char *fec;         /* create_fec() output */
    char *frame;                /* header + text + trailer of a NAVTEX message */
    size_t n_fec;
    /* the loaded message as segments: phasing, bits, trailer */
    size_t pre_samples;         /* phasing before the bits */
    size_t post_samples;        /* phasing after the bits */
    size_t bit_samples;         /* samples per bit (110) */
    size_t total, pos;
};

#endif
