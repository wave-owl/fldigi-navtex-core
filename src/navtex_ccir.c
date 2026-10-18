/*
 * fldigi-navtex-core: the CCIR 476 (4-of-7) code of fldigi 4.2.13 (src/navtex/navtex.cxx
 * class CCIR476 and the tables before it), the B-mode encoder, the message text helpers and
 * the version string.
 *
 * Copyright (C) 2026 Gerolf Ziegenhain
 * Original C++: (c) 2011-2016 Remi Chateauneu F4ECW, Rik van Riel AB1KW; JNX (c) Paul Lutus.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "navtex_internal.h"

#include <string.h>

#define STR2(x) #x
#define STR(x) STR2(x)

const char *fnavtex_version(void)
{
    return "fldigi-navtex-core " FNAVTEX_VERSION_STRING " contract=navtex-c1 abi="
           STR(FNAVTEX_ABI) " tx=1 dsp=" FDSP_VERSION_STRING;
}

int fnavtex_abi(void)
{
    return FNAVTEX_ABI;
}

/* code_to_ltrs, navtex.cxx:465-475 */
const unsigned char fnavtex_code_to_ltrs[128] = {
    /*0 1    2    3    4    5    6    7    8    9    a    b    c    d    e    f */
    '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', /* 0 */
    '_', '_', '_', '_', '_', '_', '_', 'J', '_', '_', '_', 'F', '_', 'C', 'K', '_', /* 1 */
    '_', '_', '_', '_', '_', '_', '_', 'W', '_', '_', '_', 'Y', '_', 'P', 'Q', '_', /* 2 */
    '_', '_', '_', '_', '_', 'G', '_', '_', '_', 'M', 'X', '_', 'V', '_', '_', '_', /* 3 */
    '_', '_', '_', '_', '_', '_', '_', 'A', '_', '_', '_', 'S', '_', 'I', 'U', '_', /* 4 */
    '_', '_', '_', 'D', '_', 'R', 'E', '_', '_', 'N', '_', '_', ' ', '_', '_', '_', /* 5 */
    '_', '_', '_', 'Z', '_', 'L', '_', '_', '_', 'H', '_', '_', '\n', '_', '_', '_', /* 6 */
    '_', 'O', 'B', '_', 'T', '_', '_', '_', '\r', '_', '_', '_', '_', '_', '_', '_' /* 7 */
};

/* USTTY_code_to_figs, navtex.cxx:477-487 */
const unsigned char fnavtex_ustty_code_to_figs[128] = {
    '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', /* 0 */
    '_', '_', '_', '_', '_', '_', '_', '\'', '_', '_', '_', '!', '_', ':', '(', '_', /* 1 */
    '_', '_', '_', '_', '_', '_', '_', '2', '_', '_', '_', '6', '_', '0', '1', '_', /* 2 */
    '_', '_', '_', '_', '_', '&', '_', '_', '_', '.', '/', '_', ';', '_', '_', '_', /* 3 */
    '_', '_', '_', '_', '_', '_', '_', '-', '_', '_', '_', '\07', '_', '8', '7', '_', /* 4 */
    '_', '_', '_', '$', '_', '4', '3', '_', '_', ',', '_', '_', ' ', '_', '_', '_', /* 5 */
    '_', '_', '_', '"', '_', ')', '_', '_', '_', '#', '_', '_', '\n', '_', '_', '_', /* 6 */
    '_', '9', '?', '_', '5', '_', '_', '_', '\r', '_', '_', '_', '_', '_', '_', '_' /* 7 */
};

/* ITA2_code_to_figs, navtex.cxx:489-499 */
const unsigned char fnavtex_ita2_code_to_figs[128] = {
    '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', /* 0 */
    '_', '_', '_', '_', '_', '_', '_', '\'', '_', '_', '_', '!', '_', ':', '(', '_', /* 1 */
    '_', '_', '_', '_', '_', '_', '_', '2', '_', '_', '_', '6', '_', '0', '1', '_', /* 2 */
    '_', '_', '_', '_', '_', '&', '_', '_', '_', '.', '/', '_', '=', '_', '_', '_', /* 3 */
    '_', '_', '_', '_', '_', '_', '_', '-', '_', '_', '_', '\'', '_', '8', '7', '_', /* 4 */
    '_', '_', '_', '$', '_', '4', '3', '_', '_', ',', '_', '_', ' ', '_', '_', '_', /* 5 */
    '_', '_', '_', '+', '_', ')', '_', '_', '_', '#', '_', '_', '\n', '_', '_', '_', /* 6 */
    '_', '9', '?', '_', '5', '_', '_', '_', '\r', '_', '_', '_', '_', '_', '_', '_' /* 7 */
};

int fnavtex_check_bits(int v)
{
    /* CCIR476::check_bits, navtex.cxx:593-601 (Kernighan's bit count) */
    int bc = 0;
    while (v != 0) {
        bc++;
        v &= v - 1;
    }
    return bc == 4;
}

int fnavtex_code_to_char(int code, int shift, int ita2)
{
    /* CCIR476::code_to_char, navtex.cxx:562-575 */
    const unsigned char *target;
    if (ita2)
        target = shift ? fnavtex_ita2_code_to_figs : fnavtex_code_to_ltrs;
    else
        target = shift ? fnavtex_ustty_code_to_figs : fnavtex_code_to_ltrs;
    if (target[code] != '_')
        return target[code];
    return -code;
}

int fnavtex_bytes_to_code(const int *pos)
{
    /* CCIR476::bytes_to_code, navtex.cxx:577-584 */
    int code = 0;
    int i;
    for (i = 0; i < 7; i++)
        code |= ((pos[i] > 0) << i);
    return code;
}

int fnavtex_valid_char_at(const int *pos)
{
    /* CCIR476::valid_char_at, navtex.cxx:604-613 */
    int count = 0;
    int i;
    for (i = 0; i < 7; i++)
        if (pos[i] > 0)
            count++;
    return count == 4;
}

void fnavtex_encoder_tables(int ita2, unsigned char ltrs_to_code[128],
                            unsigned char figs_to_code[128])
{
    /* CCIR476::CCIR476, navtex.cxx:516-539 (the valid-code table is check_bits itself) */
    int code;
    memset(ltrs_to_code, 0, 128);
    memset(figs_to_code, 0, 128);
    for (code = 0; code < 128; code++) {
        if (fnavtex_check_bits(code)) {
            unsigned char figv = ita2 ? fnavtex_ita2_code_to_figs[code] :
                                        fnavtex_ustty_code_to_figs[code];
            unsigned char ltrv = fnavtex_code_to_ltrs[code];
            if (figv != '_')
                figs_to_code[figv] = (unsigned char)code;
            if (ltrv != '_')
                ltrs_to_code[ltrv] = (unsigned char)code;
        }
    }
}

static int upper(int ch)
{
    /* toupper in the C locale */
    return (ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch;
}

size_t fnavtex_encode(const unsigned char ltrs_to_code[128], const unsigned char figs_to_code[128],
                      const char *text, size_t n, unsigned char *out)
{
    /* encode, navtex.cxx:1752-1760, with CCIR476::char_to_code, :541-560. fldigi indexes its
     * tables with toupper() of a plain char; bytes above 127 would read outside them and are
     * skipped here, like the characters without a code. */
    size_t i, k = 0;
    int shift = 0;
    for (i = 0; i < n; i++) {
        int ch = (unsigned char)text[i];
        if (ch > 127)
            continue;
        ch = upper(ch);
        /* avoid unnecessary shifts */
        if (shift && figs_to_code[ch] != '\0') {
            out[k++] = figs_to_code[ch];
        } else if (!shift && ltrs_to_code[ch] != '\0') {
            out[k++] = ltrs_to_code[ch];
        } else if (figs_to_code[ch] != '\0') {
            shift = 1;
            out[k++] = NAVTEX_CODE_FIGS;
            out[k++] = figs_to_code[ch];
        } else if (ltrs_to_code[ch] != '\0') {
            shift = 0;
            out[k++] = NAVTEX_CODE_LTRS;
            out[k++] = ltrs_to_code[ch];
        }
    }
    return k;
}

size_t fnavtex_create_fec(const unsigned char *str, size_t sz, unsigned char *res)
{
    /* create_fec, navtex.cxx:1728-1749: "NAUTICAL" becomes rep alpha rep alpha N alpha A
     * alpha U N T A ... (every character again five positions after its first copy). fldigi
     * reads str[sz - 2 + i] for the tail, before the buffer when sz < 2; callers refuse that. */
    const size_t offset = 2;
    size_t i, k = 0;
    for (i = 0; i < offset; ++i) {
        res[k++] = NAVTEX_CODE_REP;
        res[k++] = NAVTEX_CODE_ALPHA;
    }
    for (i = 0; i < sz; ++i) {
        res[k++] = str[i];
        res[k++] = i >= offset ? str[i - offset] : NAVTEX_CODE_ALPHA;
    }
    for (i = 0; i < offset; ++i) {
        res[k++] = NAVTEX_CODE_CHAR32;
        res[k++] = str[sz - offset + i];
    }
    return k;
}

size_t fnavtex_cleanup(const char *in, size_t n, char *out, size_t max)
{
    /* ccir_message::cleanup, navtex.cxx:663-690 */
    int was_delim = 0, was_space = 0, chr_seen = 0;
    size_t i, k = 0;
#define PUT(c) do { if (out && k + 1 < max) out[k] = (c); k++; } while (0)
    for (i = 0; i < n; i++) {
        switch (in[i]) {
        case '\n':
        case '\r':
            was_delim = 1;
            break;
        case ' ':
        case '\t':
            was_space = 1;
            break;
        default:
            if (chr_seen) {
                if (was_delim)
                    PUT('\n');
                else if (was_space)
                    PUT(' ');
            }
            was_delim = 0;
            was_space = 0;
            chr_seen = 1;
            PUT(in[i]);
        }
    }
#undef PUT
    if (out && max > 0)
        out[k < max ? k : max - 1] = '\0';
    return k;
}

const char *fnavtex_subject_name(char subject)
{
    /* ccir_message::msg_type, navtex.cxx:637-660 */
    switch (subject) {
    case 'A': return "Navigational warning";
    case 'B': return "Meteorological warning";
    case 'C': return "Ice report";
    case 'D': return "Search & rescue information, pirate warnings";
    case 'E': return "Meteorological forecast";
    case 'F': return "Pilot service message";
    case 'G': return "AIS message";
    case 'H': return "LORAN message";
    case 'I': return "Not used";
    case 'J': return "SATNAV messages";
    case 'K': return "Other electronic navaid messages";
    case 'L': return "Navigational warnings";
    case 'T': return "Test transmissions (UK only)";
    case 'V': return "Notice to fishermen (U.S. only)";
    case 'W': return "Environmental (U.S. only)";
    case 'X': return "Special services - allocation by IMO NAVTEX Panel";
    case 'Y': return "Special services - allocation by IMO NAVTEX Panel";
    case 'Z': return "No message on hand";
    default: return "Invalid navtex subject";
    }
}
