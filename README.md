# fldigi-navtex-core

[![CI](https://github.com/wave-owl/fldigi-navtex-core/actions/workflows/ci.yml/badge.svg)](https://github.com/wave-owl/fldigi-navtex-core/actions/workflows/ci.yml)

A C99 library that receives and transmits **NAVTEX** and **SITOR-B** (CCIR 476 collective
B-mode FEC, 100 baud, 170 Hz shift) exactly the way fldigi 4.2.13 does. It is a
line-by-line port of `src/navtex/navtex.cxx` (Remi Chateauneu F4ECW and Rik van Riel AB1KW,
after Paul Lutus' JNX), built on the shared
[fldigi-dsp-core](https://github.com/wave-owl/fldigi-dsp-core) (fldigi's overlap-add FFT
filter with its raised-cosine RTTY shape, complex FFT). On the reference build the
transmitter reproduces fldigi's TX samples bit for bit, and the receiver prints fldigi's
characters, logs fldigi's messages and computes fldigi's logic levels bit for bit: clean,
in noise down to -12 dB, 10 Hz off frequency, across the framing corner cases and on pure
noise (see [Verification](#verification)). It is the NAVTEX implementation of
[wave-owl](https://github.com/wave-owl/wave-owl).

| | |
|---|---|
| **Is** | A single-channel **receiver**: one event per character fldigi prints (with a flag for characters recovered by the FEC cascade), and one record per message fldigi logs: B1 origin, B2 subject, B3B4 number, how it ended (NNNN, next header, 600 s timeout, lost header) and fldigi's logged text. fldigi's s/n and metric, optional AFC. |
| **Is** | fldigi's **transmitter** (capability tag `tx=1`): NAVTEX framing (phasing, `ZCZC ZI<nn>`, text, `NNNN`, phasing) or plain SITOR-B, the letters/figures encoder and the B-mode interleave of every character with its repetition. |
| **Is not** | A NAVTEX station: no station list, no message archive, no schedule (wave-owl does that). No GUI waterfall: AFC and s/n use a power spectrum computed here. Not a skimmer: one carrier per instance. |
| | An I/O layer, threaded or allocating at run time: no sound card or file access, no threads, all memory allocated in `*_create()`, no global mutable state. |

**Transmission note.** 490, 518 and 4209.5 kHz are maritime safety frequencies. This library
only turns text into audio; where a host may transmit is the host's business. wave-owl
transmits SITOR-B only inside amateur-band digital segments, never with a NAVTEX header
(wave-owl ADR D-894).

## Contents

- [NAVTEX in one paragraph](#navtex-in-one-paragraph)
- [Signal chain](#signal-chain)
- [fldigi source map](#fldigi-source-map)
- [fldigi behaviour kept on purpose](#fldigi-behaviour-kept-on-purpose)
- [Deliberate deviations](#deliberate-deviations)
- [API reference](#api-reference)
- [Usage example](#usage-example)
- [Contract and versioning](#contract-and-versioning)
- [Verification](#verification)
- [Performance and memory](#performance-and-memory)
- [Building](#building)
- [License and credits](#license-and-credits)

## NAVTEX in one paragraph

SITOR-B sends 7-bit CCIR 476 characters with exactly four ones (35 valid codes: letters,
figures through a shift, space, CR, LF and six service codes) as 100 baud FSK, mark
(1, upper tone) and space 170 Hz apart. Every character is sent twice: the "rep" copy and,
five characters (35 bits) later, the "alpha" copy, so the receiver can repair a character
from its other copy. NAVTEX frames the stream: at least 10 s of phasing, `ZCZC`, a space,
B1 (station), B2 (subject: A navigational warning, B meteorological warning, E forecast,
...), B3B4 (serial number), CR LF, the text, `NNNN`.

## Signal chain

Receiver, per sample at 11025 Hz (fldigi's modem rate), in fldigi's sound-card blocks of
512 samples:

1. `x * 32767` as a complex number (x, x), mixed with `cos/sin` of the mark (carrier + 85 Hz)
   and space (carrier - 85 Hz) phases (`mixer`, `navtex.cxx:1559-1567`).
2. Two 512-point fftfilt low passes with fldigi's raised-cosine RTTY shape for 100 baud
   (`configure_filters`, `:994-1006`; fldigi-dsp-core `fdsp_fftfilt_rtty_filter`).
3. |mark|, |space| (hypot); envelopes (fast up, slow down) and noise floors (fast down, slow
   up); W7AY's discriminator with automatic threshold correction
   (`process_fft_output`, `:1589-1670`).
4. The early/prompt/late multicorrelator (a fifth of a bit apart) moves the sampling every
   8 bits (`process_multicorrelator`, `:1480-1505`); the prompt value is the bit's
   confidence.
5. A 100-bit window: the alpha/rep phase search (`find_alpha_characters`, `:1129-1187`), then
   every second 7-bit character through the FEC cascade: the alpha copy, the rep copy, the
   sum of both, a flipped weakest bit in alpha, in rep, in the sum (`process_bytes`,
   `:1238-1323`).
6. Letters/figures shift and the service codes (`process_char`, `:1325-1362`), the printed
   character (`filter_print`, `:1364-1371`) and the message framing: header
   `ZCZC B1B2B3B4`, trailer `NNNN`, lost header, lost trailer, 600 s timeout
   (`process_messages`, `:1045-1078`; `ccir_message`, `:624-858`).

Transmitter: `encode` (letters/figures, `:1752-1760`), `create_fec` (`:1728-1749`), seven bits
per code least significant first, each bit 110 samples of `0.9 sin` at mark or space with one
continuous phase (`send_sine`, `:1781-1792`), and in NAVTEX mode phasing at the carrier
around it (`send_message`, `:1822-1854`).

## fldigi source map

| fldigi 4.2.13 | This library |
|---|---|
| `navtex.cxx:465-499` code tables, `:502-508` service codes | `src/navtex_ccir.c`, `src/navtex_internal.h` |
| `navtex.cxx:510-615` class `CCIR476` | `fnavtex_check_bits`, `fnavtex_code_to_char`, `fnavtex_bytes_to_code`, `fnavtex_valid_char_at`, `fnavtex_encoder_tables` |
| `navtex.cxx:624-858` class `ccir_message` | `process_messages`, `detect_end`, `fnavtex_cleanup`, `fnavtex_subject_name` (`src/navtex_rx.c`, `src/navtex_ccir.c`) |
| `navtex.cxx:942-1078` ctor, filters, flush, timeout, messages | `src/navtex_rx.c` |
| `navtex.cxx:1080-1371` FEC cascade, characters | `fnavtex_i_process_bytes` and neighbours |
| `navtex.cxx:1373-1464` metric, AFC | `compute_metric`, `process_afc` |
| `navtex.cxx:1480-1670` bit sync, demodulator | `process_multicorrelator`, `process_fft_output`, `process_data` |
| `navtex.cxx:1683-1698` display_message | `display_message` (the logged text) |
| `navtex.cxx:1728-1854` encoder, FEC, send_* | `src/navtex_tx.c` |
| `fftfilt.cxx:243-290` rtty_filter | fldigi-dsp-core 0.7.0 `fdsp_fftfilt_rtty_filter` |
| `waterfall.cxx:520-576, 599-638` power spectrum | `wf_update`, `power_density`, `power_density_maximum` |

## fldigi behaviour kept on purpose

- **Bit length.** The receiver tracks 110.25 samples per bit (`m_bit_sample_count`), the
  transmitter sends 110 (`int nb_samples = 0.01 * 11025`).
- **FEC quirks.** `flip_smallest_bit` starts counting ones at 1 and modifies the stored bits
  in place; a rep copy that decodes to `code_rep` skips the character (`process_bytes`).
- **Cleaned cut, raw message.** The text before a new header goes out cleaned (CR/LF runs to
  one LF, blanks to one space, `ccir_message` ctor) and its emptiness is tested after
  cleaning; a message flushed by `NNNN` or the timeout keeps its raw characters. Both carry
  fldigi's decorations (`[Lost header]:`, `:[Lost trailer]`, `:<TIMEOUT>`).
- **Empty timeouts.** Every 600 s without a message fldigi logs `[Lost header]::<TIMEOUT>`
  (body length 0); so does the library. SITOR-B mode never logs messages.
- **NAVTEX TX header.** fldigi frames every NAVTEX message as `ZCZC ZI<nn>` (origin Z "never
  seen", subject I "not used") with a counter that starts at 1, and adds `\r\nNNNN\r\n\n`.
- **US-TTY figures** by default (`ITA2` off): `=` and `+` have no figures code there and are
  skipped by the encoder; BEL prints as an apostrophe.
- **s/n.** fldigi compares the mark and space power densities with 240 times the density at
  the carrier, where FSK sidebands sit, so a clean signal reads about -15 dB.

## Deliberate deviations

| fldigi | Here | Why |
|---|---|---|
| function-static variables (envelopes, noise floors, `last_char`, metric, AFC counters, TX phase) shared by all instances | per instance | several receivers at once |
| `int m_sample_count`, wraps after 2^31 samples (2.25 days) | 64-bit counter | a 24/7 monitor; tested across 2^31 |
| AFC and s/n from the GUI waterfall (8 kHz, resampled) | Blackman FFT of the last 8192 samples at 11025 Hz, 1 Hz columns, updated every block; fldigi's `powerDensity` / `powerDensityMaximum` on it | no GUI; AFC is off in fldigi's bench, so it is tested by function |
| waterfall "reverse" and the modem reverse flag | USB only | not meaningful without the GUI |
| double to int and int sums undefined outside the int range | saturate / wrap as on fldigi's reference build (arm64) | defined behaviour, same results |
| `std::string` message of unbounded length | 8192-byte buffer (a NAVTEX message cannot reach it before the timeout; SITOR-B drops its never-shown oldest half) | no allocation after create |
| `create_fec` reads before its buffer for fewer than two codes | `fnavtex_tx_load` refuses such a SITOR-B text | no undefined behaviour |
| bytes above 127 index outside the encoder tables | skipped | no undefined behaviour |

## API reference

See `include/fldigi_navtex.h` (every function documented there). In short:

| Function | |
|---|---|
| `fnavtex_version()`, `fnavtex_abi()` | `"fldigi-navtex-core 0.1.0 contract=navtex-c1 abi=1 tx=1 dsp=0.7.0"`, `1` |
| `fnavtex_sizeof(what)` | `sizeof` of the event, message and config structures, so a binding can check its mirrors |
| `fnavtex_rx_create(cfg)` / `_destroy` / `_reset` | mode NAVTEX or SITOR-B, carrier (200 .. 3000 Hz, 0 = 1000), AFC, ITA2, minimum logged size |
| `fnavtex_rx_process(rx, x, n, ev, max)` | 11025 Hz samples (int16 / 32768) in, printed characters out; complete 512-sample blocks are processed |
| `fnavtex_rx_flush` | process the waiting partial block (end of input) |
| `fnavtex_rx_pending`, `fnavtex_rx_take_message` | logged messages (16 kept) |
| `fnavtex_rx_snr_db`, `_metric`, `_carrier_hz`, `_set_carrier`, `_state`, `_samples`, `_dropped`, `_footprint`, `_take_bits` | status and test taps |
| `fnavtex_cleanup`, `fnavtex_subject_name` | fldigi's log cleanup and B2 names |
| `fnavtex_tx_create(cfg)` / `_destroy` | mode, carrier, ITA2, first header number |
| `fnavtex_tx_load(tx, text, n, is_first, is_last)` | load one message, returns its length in samples (0: refused) |
| `fnavtex_tx_render(tx, out, max)` | the next samples (peak 0.9, before fldigi's -12 dB TX level) |
| `fnavtex_tx_remaining`, `_next_number`, `_codes` | status and test tap |

## Usage example

```c
#include "fldigi_navtex.h"

fnavtex_rx_config c = {FNAVTEX_MODE_NAVTEX, 1000.0, 0, 0, 0};
fnavtex_rx *rx = fnavtex_rx_create(&c);
fnavtex_event ev[256];
size_t n = fnavtex_rx_process(rx, samples, count, ev, 256);   /* 11025 Hz */
for (size_t i = 0; i < n; i++)
    putchar(ev[i].ch);
fnavtex_message m;
char text[9000];
while (fnavtex_rx_take_message(rx, &m, text, sizeof text))
    printf("%c%c%02d %s: %s\n", m.origin, m.subject, m.number,
           fnavtex_subject_name(m.subject), text);
fnavtex_rx_destroy(rx);
```

## Contract and versioning

The contract tag `navtex-c1` and `abi=1` change only with an incompatible API change; wave-owl
refuses a library with other tags. `tx=1` announces the transmitter. Versions follow semantic
versioning. The pinned fldigi-dsp-core (`dsp/`, `dsp.lock`: version, commit and the checksum
of the linked files) is checked by `tools/check_dsp.sh` before every build.

## Verification

`make test` runs two programs, `make sanitize` the same under AddressSanitizer and
UndefinedBehaviorSanitizer; CI runs both on Ubuntu and macOS.

- `tests/test_navtex.c`: the CCIR 476 tables (35 valid codes), the encoder and `create_fec`
  (fldigi's "NAUTICAL" example), every branch of the FEC cascade, the phase search, the
  framing (complete, lost header, lost trailer, cleaned cut, timeout, empty timeout, minimum
  size, SITOR-B, queue bound), chunk invariance (1 .. 100000 samples per call, reset),
  loopback TX -> RX for both modes, carriers 600 .. 2200 Hz and ITA2, 120 s of noise (no
  text), decoding across 2^31 samples, AFC, the s/n estimate, the 99 % occupied bandwidth
  (315.8 Hz for a SITOR-B burst).
- `tests/test_navtex_fldigi.c`: goldens from the fldigi 4.2.13 bench build
  (`tests/golden/make_fldigi_golden.py`, dump hooks `tests/golden/fldigi-bench-navtex.patch`):
  11 TX cases (NAVTEX and SITOR-B, CQ, pangram with figures, multi-line, lower case, ITA2,
  1500 Hz, the framing streams) and 38 RX decodes (clean, -3, -6, -9, -12 dB in 2500 Hz,
  10 Hz mistuned, back to back, lost header, lost trailer, 600 s timeout, SITOR-B received
  as NAVTEX, 20 s of noise).

| | Result (macOS arm64, Apple clang, `-O3` and the `-O1` sanitizer build) |
|---|---|
| TX: doubles before the TX level and the int16 file identical to fldigi | 11 of 11 |
| RX: printed bytes, logged messages and logic levels identical to fldigi | 38 of 38 |

fldigi's own decode limit in these vectors lies between -6 and -9 dB (2500 Hz). On other
platforms (another libm, `hypot`/`sin` differing in the last bit) the results need not be
bit-identical; there the clean decodes must stay within 1 + 5 % edit distance of fldigi's and
the noisy ones within 20 % + 10 in total, and the TX within 1 LSB of the int16 vectors.

## Performance and memory

`make bench` (Apple M4, one core): receiver about 950 x real time (including the power
spectrum), transmitter about 25 000 x real time; 604 kB per receiver (message queue 16 x 8 kB,
spectrum buffers, two 512-point filters).

## Building

```
git clone --recursive https://github.com/wave-owl/fldigi-navtex-core
make            # build/libfldigi_navtex.a and .dylib/.so
make test       # unit tests and fldigi goldens
make sanitize   # the same under ASan + UBSan
make bench
```

Regenerating the goldens needs the fldigi bench build (wave-owl's archived change
`fix-psk-rtty-vs-fldigi`, `bench/BUILD.md`) with `tests/golden/fldigi-bench-navtex.patch`:

```
FLDIGI_BENCH_BIN=/path/to/src-bench/src/fldigi python3 tests/golden/make_fldigi_golden.py
```

## License and credits

GPL-3.0-or-later, like fldigi. Ported from fldigi 4.2.13 (mirror
[wave-owl/fldigi](https://github.com/wave-owl/fldigi), tag v4.2.13, commit
`e7c3ab3709ad62a7d91829364eb48a05fa325c0d`): NAVTEX (c) 2011-2016 Remi Chateauneu F4ECW and
Rik van Riel AB1KW, adapted from JNX (c) Paul Lutus; fftfilt and waterfall (c) Dave Freese
W1HKJ. C port (c) 2026 Gerolf Ziegenhain.
