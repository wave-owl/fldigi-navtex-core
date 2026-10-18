#!/usr/bin/env python3
"""Golden data from fldigi 4.2.13 itself for fldigi-navtex-core (stdlib only).

Drives the headless fldigi bench build (configure --enable-benchmark plus the TX extension
of wave-owl's archived change fix-psk-rtty-vs-fldigi, bench/BUILD.md, and the NAVTEX dump
hooks of wave-owl's change add-fldigi-navtex-core, tests/golden/fldigi-bench-navtex.patch):

* TX: fldigi renders the text (bench-tx, txlevel -12 dB, PCM16, 11025 Hz) in NAVTEX mode
  (trx_mode 51: phasing, "ZCZC ZI01", text, "NNNN", phasing) or SITOR-B mode (52: the text
  alone). FLDIGI_BENCH_NAVTEX_TXDUMP records the samples fldigi hands to ModulateXmtr before
  the TX level; tx_hashes.txt keeps the FNV-1a 64 hash of those doubles and of the int16
  file. Vectors other cases receive are kept as raw little-endian int16 in tx/<stem>.s16.
* RX: fldigi receives 1 s silence + the source vector(s) + the tail, clean and buried in
  deterministic noise (2500 Hz reference bandwidth, see noisy()). Every byte fldigi prints
  goes to rx/<stem>_<snr>.txt, every message it logs (FLDIGI_BENCH_NAVTEX_MSG) to
  rx/<stem>_<snr>.msg, and the FNV-1a hash of the logic levels handed to handle_bit_value
  (FLDIGI_BENCH_NAVTEX_BITS) to rx_bits.txt. Pure noise (20 s) goes to rx/<mode>_noise.*.

    FLDIGI_BENCH_BIN=/path/to/src-bench/src/fldigi python3 tests/golden/make_fldigi_golden.py

The C tests (tests/test_navtex_fldigi.c) rebuild the same inputs bit for bit.
SPDX-License-Identifier: GPL-3.0-or-later
"""

from __future__ import annotations

import array
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
FLDIGI = os.environ.get("FLDIGI_BENCH_BIN", "fldigi")
SR = 11025
MODE_ID = {"NAVTEX": 51, "SITORB": 52}   # trx_mode ids of fldigi 4.2.13 (globals.h)
# '\n' in a text is a line feed; texts.txt stores it as the two characters backslash n.
TEXTS = {
    "cq": "CQ CQ DE DG6FL DG6FL K",
    "pangram": "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 ?-/().,':=+",
    "wx": "WEATHER FORECAST\nWIND SW 4 TO 5\nSEA STATE 3 M",
    "lower": "cq de dg6fl dg6fl k",
    "lost_header": "TEST WITHOUT HEADER\nNNNN\n",
    "lost_trailer": "ZCZC AA01\nFIRST MESSAGE\nZCZC AB02\nSECOND MESSAGE\nNNNN\n",
    "timeout": "ZCZC AA03\nMESSAGE WITHOUT END ",
}
# k = 10^(-SNR/10) for SNR = -3 .. -12 dB (2500 Hz), as literals so C uses the very same
# double; fldigi's NAVTEX threshold lies between -6 and -9 dB
SNRS = (("clean", 0.0), ("m03", 1.9952623149688795), ("m06", 3.9810717055349722),
        ("m09", 7.943282347242816), ("m12", 15.848931924611133))
ALL = tuple(n for n, _ in SNRS)


def tx_cases():
    """(stem, mode, text key, carrier, ita2, stored)"""
    return [
        ("navtex_cq", "NAVTEX", "cq", 1000, 0, True),
        ("navtex_pangram", "NAVTEX", "pangram", 1000, 0, True),
        ("navtex_wx", "NAVTEX", "wx", 1000, 0, True),
        ("navtex_lower", "NAVTEX", "lower", 1000, 0, False),
        ("navtex_cq_1500", "NAVTEX", "cq", 1500, 0, True),
        ("navtex_ita2", "NAVTEX", "pangram", 1000, 1, True),
        ("sitorb_cq", "SITORB", "cq", 1000, 0, True),
        ("sitorb_pangram", "SITORB", "pangram", 1000, 0, True),
        ("sitorb_lost_header", "SITORB", "lost_header", 1000, 0, True),
        ("sitorb_lost_trailer", "SITORB", "lost_trailer", 1000, 0, True),
        ("sitorb_timeout", "SITORB", "timeout", 1000, 0, True),
    ]


def rx_cases():
    """(stem, sources joined with '+', rx mode, rx carrier, ita2, snrs, tail seconds)"""
    return [
        ("navtex_cq", "navtex_cq", "NAVTEX", 1000, 0, ALL, 3),
        ("navtex_pangram", "navtex_pangram", "NAVTEX", 1000, 0, ALL, 3),
        ("navtex_wx", "navtex_wx", "NAVTEX", 1000, 0, ("clean", "m06", "m09"), 3),
        ("navtex_cq_1500", "navtex_cq_1500", "NAVTEX", 1500, 0, ("clean", "m06"), 3),
        ("navtex_ita2", "navtex_ita2", "NAVTEX", 1000, 1, ("clean", "m06"), 3),
        ("navtex_cq_rx1010", "navtex_cq", "NAVTEX", 1010, 0, ("clean", "m06"), 3),
        ("navtex_cq_rx990", "navtex_cq", "NAVTEX", 990, 0, ("clean", "m06"), 3),
        ("navtex_two", "navtex_cq+navtex_pangram", "NAVTEX", 1000, 0, ("clean", "m06"), 3),
        ("sitorb_cq", "sitorb_cq", "SITORB", 1000, 0, ALL, 3),
        ("sitorb_pangram", "sitorb_pangram", "SITORB", 1000, 0, ("clean", "m06"), 3),
        ("sitorb_cq_as_navtex", "sitorb_cq", "NAVTEX", 1000, 0, ("clean",), 3),
        ("lost_header", "sitorb_lost_header", "NAVTEX", 1000, 0, ("clean", "m06"), 3),
        ("lost_trailer", "sitorb_lost_trailer", "NAVTEX", 1000, 0, ("clean", "m06"), 3),
        ("timeout", "sitorb_timeout", "NAVTEX", 1000, 0, ("clean",), 620),
    ]


def config_xml(ita2=0):
    return f"<FLDIGI_DEFS>\n<ITA2>{ita2}</ITA2>\n</FLDIGI_DEFS>\n"


def run_fldigi(mode, carrier, args, xml, env_extra=None):
    cfg = tempfile.mkdtemp(prefix="fldigi-navtex-golden-")
    Path(cfg, "fldigi_def.xml").write_text(xml)
    env = {k: v for k, v in os.environ.items() if not k.startswith("FLDIGI_BENCH_")}
    # re-apply the carrier after the modem's init, as a click on the waterfall does: the
    # navtex modem starts at 1000 Hz and takes --benchmark-frequency only through set_freq
    env["FLDIGI_BENCH_RETUNE"] = "1"
    env.update(env_extra or {})
    try:
        p = subprocess.run([FLDIGI, "--config-dir", cfg, "--benchmark-modem",
                            str(MODE_ID[mode]), "--benchmark-frequency", str(carrier),
                            *args], env=env, capture_output=True, text=True, timeout=3600)
    finally:
        shutil.rmtree(cfg, ignore_errors=True)
    if f"mode={mode} " not in p.stderr:
        raise RuntimeError(f"fldigi did not run {mode}: {p.stderr[-400:]}")
    return p.stderr


def read_wav(path):
    with wave.open(str(path)) as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2
        sr = w.getframerate()
        a = array.array("h", w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        a.byteswap()
    return sr, a


def write_wav(path, sr, pcm):
    a = array.array("h", pcm)
    if sys.byteorder == "big":
        a.byteswap()
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(a.tobytes())


def fnv1a64_bytes(data, h=0xCBF29CE484222325):
    for byte in data:
        h ^= byte
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def fnv1a64(pcm):
    raw = array.array("h", pcm)
    if sys.byteorder == "big":
        raw.byteswap()
    return fnv1a64_bytes(raw.tobytes())


class Lcg:
    """32-bit LCG (Numerical Recipes); the same in tests/test_navtex_fldigi.c."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF

    def uniform(self):
        self.s = (self.s * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.s / 4294967296.0


def noisy(pcm, sr, k, seed, tail_s):
    """1 s + pcm + tail_s s, plus Irwin-Hall Gaussian noise; k = 10^(-SNR/10) (0: clean).

    Only +, -, *, / and sqrt (all correctly rounded), so C reproduces it bit for bit.
    SNR = mean square of the burst / (noise variance * 2500 / (sr / 2)).
    """
    padded = [0.0] * sr + [float(v) for v in pcm] + [0.0] * (tail_s * sr)
    if k == 0.0:
        return [int(v) for v in padded]
    ps = 0.0
    for v in pcm:
        ps += float(v) * float(v)
    ps = ps / len(pcm)
    s2 = ps * k * (sr / 2.0) / 2500.0
    sd = math.sqrt(s2)
    gain = 3276.7 / math.sqrt(ps * len(pcm) / len(padded) + s2)
    rng = Lcg(seed)
    out = []
    for v in padded:
        z = 0.0
        for _ in range(12):
            z += rng.uniform()
        z -= 6.0
        y = (v + sd * z) * gain
        q = math.floor(y + 0.5)
        out.append(max(-32768, min(32767, int(q))))
    return out


def noise_only(sr, seconds, seed):
    """seconds of Irwin-Hall noise at 0.1 RMS (3276.7 int16 units)."""
    rng = Lcg(seed)
    out = []
    for _ in range(seconds * sr):
        z = 0.0
        for _ in range(12):
            z += rng.uniform()
        z -= 6.0
        out.append(max(-32768, min(32767, int(math.floor(z * 3276.7 + 0.5)))))
    return out


def load_s16(stem):
    raw = array.array("h", Path(HERE, "tx", stem + ".s16").read_bytes())
    if sys.byteorder == "big":
        raw.byteswap()
    return raw


def one_tx(idx, case):
    stem, mode, key, carrier, ita2, stored = case
    with tempfile.TemporaryDirectory() as td:
        wav = Path(td, stem + ".wav")
        dump = Path(td, stem + ".f64")
        run_fldigi(mode, carrier, ["--benchmark-input", "1"], config_xml(ita2),
                   {"FLDIGI_BENCH_TX_TEXT": TEXTS[key], "FLDIGI_BENCH_TX_WAV": str(wav),
                    "FLDIGI_BENCH_NAVTEX_TXDUMP": str(dump)})
        sr, pcm = read_wav(wav)
        doubles = dump.read_bytes()
    assert sr == SR and len(doubles) == 8 * len(pcm)
    if stored:
        raw = array.array("h", pcm)
        if sys.byteorder == "big":
            raw.byteswap()
        Path(HERE, "tx", stem + ".s16").write_bytes(raw.tobytes())
    line = (f"{stem} {mode} {key} {idx} {carrier} {ita2} {len(pcm)} "
            f"{fnv1a64(pcm):016x} {fnv1a64_bytes(doubles):016x}")
    print(line, flush=True)
    return line


def read_bits(path):
    """FNV-1a over the "<sample> <level>\\n" lines fldigi wrote, and their count."""
    if not path.exists():
        return 0, fnv1a64_bytes(b"")
    data = path.read_bytes()
    return data.count(b"\n"), fnv1a64_bytes(data)


def one_rx(idx, case):
    stem, srcs, mode, carrier, ita2, snrs, tail = case
    pcm = array.array("h")
    for s in srcs.split("+"):
        pcm.extend(load_s16(s))
    bits_lines = []
    with tempfile.TemporaryDirectory() as td:
        for i, (snr, k) in enumerate(SNRS):
            if snr not in snrs:
                continue
            inp = Path(td, f"{stem}_{snr}.wav")
            write_wav(inp, SR, noisy(pcm, SR, k, 1000 * idx + i, tail))
            out = Path(td, f"{stem}_{snr}.txt")
            msg = Path(td, f"{stem}_{snr}.msg")
            bits = Path(td, f"{stem}_{snr}.bits")
            run_fldigi(mode, carrier, ["--benchmark-input", str(inp), "--benchmark-output",
                                       str(out)], config_xml(ita2),
                       {"FLDIGI_BENCH_NAVTEX_MSG": str(msg),
                        "FLDIGI_BENCH_NAVTEX_BITS": str(bits)})
            Path(HERE, "rx", f"{stem}_{snr}.txt").write_bytes(
                out.read_bytes() if out.exists() else b"")
            Path(HERE, "rx", f"{stem}_{snr}.msg").write_bytes(
                msg.read_bytes() if msg.exists() else b"")
            n, h = read_bits(bits)
            bits_lines.append(f"{stem}_{snr} {n} {h:016x}")
    line = f"{stem} {srcs} {mode} {idx} {carrier} {ita2} {tail} {','.join(snrs)}"
    print(line, flush=True)
    return line, bits_lines


def one_noise(mode, seed):
    with tempfile.TemporaryDirectory() as td:
        inp = Path(td, "noise.wav")
        write_wav(inp, SR, noise_only(SR, 20, seed))
        out = Path(td, "noise.txt")
        msg = Path(td, "noise.msg")
        bits = Path(td, "noise.bits")
        run_fldigi(mode, 1000, ["--benchmark-input", str(inp), "--benchmark-output", str(out)],
                   config_xml(), {"FLDIGI_BENCH_NAVTEX_MSG": str(msg),
                                  "FLDIGI_BENCH_NAVTEX_BITS": str(bits)})
        data = out.read_bytes() if out.exists() else b""
        mdata = msg.read_bytes() if msg.exists() else b""
        n, h = read_bits(bits)
    Path(HERE, "rx", f"{mode.lower()}_noise.txt").write_bytes(data)
    Path(HERE, "rx", f"{mode.lower()}_noise.msg").write_bytes(mdata)
    return f"{mode.lower()}_noise {mode} {seed} {len(data)}", f"{mode.lower()}_noise {n} {h:016x}"


def main():
    (HERE / "tx").mkdir(exist_ok=True)
    (HERE / "rx").mkdir(exist_ok=True)
    jobs = int(os.environ.get("JOBS", "8"))
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        tx_lines = list(ex.map(lambda a: one_tx(*a), enumerate(tx_cases())))
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        rx_res = list(ex.map(lambda a: one_rx(*a), enumerate(rx_cases())))
        noise_res = list(ex.map(lambda a: one_noise(*a), (("NAVTEX", 77), ("SITORB", 78))))
    (HERE / "tx_hashes.txt").write_text(
        "# stem mode text index carrier ita2 samples fnv1a64(int16 LE) fnv1a64(doubles before "
        "the TX level) -- fldigi 4.2.13 bench TX, txlevel -12 dB\n" + "\n".join(tx_lines) + "\n")
    (HERE / "rx_cases.txt").write_text(
        "# stem sources mode index rx-carrier ita2 tail-seconds snrs -- fldigi 4.2.13 bench RX "
        "of 1 s + sources + tail, AFC off; noise seed = 1000 * index + snr step\n"
        + "\n".join(r[0] for r in rx_res) + "\n")
    (HERE / "rx_bits.txt").write_text(
        "# case lines fnv1a64 of fldigi's \"<sample> <level>\\n\" lines (handle_bit_value)\n"
        + "\n".join(b for r in rx_res for b in r[1]) + "\n"
        + "\n".join(r[1] for r in noise_res) + "\n")
    (HERE / "noise_cases.txt").write_text(
        "# stem mode seed bytes -- fldigi 4.2.13 bench RX of 20 s noise at 0.1 RMS\n"
        + "\n".join(r[0] for r in noise_res) + "\n")
    (HERE / "texts.txt").write_text(
        "".join(f"{k} {v.encode('unicode_escape').decode()}\n" for k, v in TEXTS.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
