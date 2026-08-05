#!/usr/bin/env python3
"""wavcheck.py -- turn a QEMU -audiodev wav capture into evidence.

The audio arc's measuring instrument. QEMU's `wav` audio backend writes the
emulated codec's output to a real WAV file, which is the only way to verify
"sound came out" from a headless harness -- the audio equivalent of a
screendump.

TRAP THIS HANDLES: the wav backend patches the RIFF/data sizes in the header
only when it closes cleanly. Our harnesses `taskkill /F` QEMU, so the sizes
are usually left at 0 and every stock WAV reader reports "0 frames" on a file
that is full of perfectly good PCM. So: parse the header for the FORMAT, but
derive the frame count from the actual file size on disk, never from the
declared size.

PROPERTIES OF THE INSTRUMENT, measured in slice 1 -- read these before
drawing conclusions from a capture:

  1. THE WAV TIMELINE IS NOT WALL-CLOCK. QEMU writes samples only while a
     stream is actually running; idle gaps are ELIDED, not recorded as
     silence. Slice 1 ran the 250 ms tone test three times and got ONE
     contiguous 729 ms run, not three bursts separated by seconds. So you
     cannot time-correlate a capture against the boot log, and "duration"
     means "total time audio was being emitted".

  2. CONCATENATED RUNS HAVE PHASE DISCONTINUITIES at the joins, which smear
     the spectrum. Measure inside a single run when precision matters.

  3. PREFER ZERO-CROSSING over the Goertzel sweep for square/pulse waves.
     On slice 1's capture the sweep wandered 860-907 Hz across windows while
     zero-crossing held 861-884 Hz and converged with window length.

  4. A LOOPED BUFFER THAT DOESN'T HOLD A WHOLE NUMBER OF PERIODS reads LOW.
     The tone test loops 1024 frames @48 kHz containing 1024/54 = 18.96
     periods, so every wrap truncates a partial period: expect ~-2% on a
     signal that is otherwise exactly 48000/54 = 888.9 Hz. That is the test
     signal's property, NOT a fault in the capture path. Budget tolerance
     accordingly (or generate whole-period buffers, which slice 2 should).

Usage:
    wavcheck.py <file.wav> [--expect-hz 880] [--min-ms 100] [--tol 0.06]

Prints a one-line [WAVCHK] verdict plus detail lines. Exit 0 = PASS.
"""
import math
import struct
import sys


def parse_wav(path):
    """Return (params, pcm_bytes). Tolerates an unfinalized header."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 44 or raw[0:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file (%d bytes)" % len(raw))

    fmt = None
    data_off = None
    declared = None
    pos = 12
    # Walk chunks. A zero-sized chunk means the header was never finalized,
    # so stop walking and treat the rest of the file as data.
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        csz = struct.unpack_from("<I", raw, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt ":
            tag, ch, rate, _brate, _align, bits = struct.unpack_from(
                "<HHIIHH", raw, body)
            fmt = dict(tag=tag, channels=ch, rate=rate, bits=bits)
        elif cid == b"data":
            data_off = body
            declared = csz
            break
        if csz == 0:
            break
        pos = body + csz + (csz & 1)

    if fmt is None:
        raise ValueError("no fmt chunk")
    if data_off is None:
        raise ValueError("no data chunk")

    # THE POINT: trust the file, not the declared size.
    actual = len(raw) - data_off
    fmt["declared_bytes"] = declared
    fmt["actual_bytes"] = actual
    fmt["finalized"] = (declared == actual and declared != 0)
    return fmt, raw[data_off:data_off + actual]


def to_mono(pcm, channels, bits):
    if bits != 16:
        raise ValueError("only 16-bit PCM supported, got %d" % bits)
    n = len(pcm) // 2
    samples = struct.unpack_from("<%dh" % n, pcm, 0)
    if channels <= 1:
        return list(samples)
    # Average the channels; a square wave is identical on both, and
    # averaging kills any anti-phase noise.
    frames = n // channels
    return [sum(samples[i * channels:(i + 1) * channels]) / channels
            for i in range(frames)]


def goertzel(x, rate, freq):
    """Single-bin DFT magnitude at `freq`, normalized to signal RMS."""
    n = len(x)
    if n == 0:
        return 0.0
    k = 2.0 * math.cos(2.0 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + k * s1 - s2
        s2, s1 = s1, s0
    power = s1 * s1 + s2 * s2 - k * s1 * s2
    return math.sqrt(max(power, 0.0)) / n


def zc_freq(x, rate, hyst=200):
    """Schmitt-triggered zero-crossing frequency. Returns (hz, crossings).

    More robust than the Goertzel sweep for square/pulse waves, and immune
    to the ringing a resampler adds. The hysteresis band rejects the small
    oscillations around zero that 48k->44.1k conversion introduces, which
    would otherwise be counted as extra crossings and double the estimate.
    """
    state = 0
    crossings = 0
    first = last = None
    for i, v in enumerate(x):
        if state <= 0 and v > hyst:
            state = 1
        elif state >= 0 and v < -hyst:
            state = -1
        else:
            continue
        crossings += 1
        if first is None:
            first = i
        last = i
    if first is None or last <= first or crossings < 3:
        return 0.0, crossings
    return rate * (crossings - 1) / (2.0 * (last - first)), crossings


def dominant_freq(x, rate, lo=50.0, hi=8000.0):
    """Coarse-to-fine Goertzel sweep. Good enough to name a test tone."""
    best_f, best_m = 0.0, -1.0
    f = lo
    while f <= hi:                       # coarse: ~1.5% steps
        m = goertzel(x, rate, f)
        if m > best_m:
            best_f, best_m = f, m
        f *= 1.015
    lo2, hi2 = best_f / 1.02, best_f * 1.02
    steps = 40
    for i in range(steps + 1):           # fine: refine the winner
        f = lo2 + (hi2 - lo2) * i / steps
        m = goertzel(x, rate, f)
        if m > best_m:
            best_f, best_m = f, m
    return best_f, best_m


def main():
    args = sys.argv[1:]
    if not args:
        print("usage: wavcheck.py <file.wav> [--expect-hz N] "
              "[--min-ms N] [--tol F]")
        return 2
    path = args[0]
    expect_hz = None
    min_ms = 50.0
    tol = 0.06
    i = 1
    while i < len(args):
        if args[i] == "--expect-hz":
            expect_hz = float(args[i + 1]); i += 2
        elif args[i] == "--min-ms":
            min_ms = float(args[i + 1]); i += 2
        elif args[i] == "--tol":
            tol = float(args[i + 1]); i += 2
        else:
            print("unknown arg %s" % args[i]); return 2

    try:
        fmt, pcm = parse_wav(path)
    except Exception as e:                        # noqa: BLE001
        print("[WAVCHK] FAIL -- %s" % e)
        return 1

    rate, ch, bits = fmt["rate"], fmt["channels"], fmt["bits"]
    print("[WAVCHK] format: %d Hz, %d ch, %d-bit  (header %s)"
          % (rate, ch, bits,
             "finalized" if fmt["finalized"]
             else "UNFINALIZED -- using on-disk size"))
    print("[WAVCHK] data: declared=%s actual=%d bytes"
          % (fmt["declared_bytes"], fmt["actual_bytes"]))

    try:
        mono = to_mono(pcm, ch, bits)
    except Exception as e:                        # noqa: BLE001
        print("[WAVCHK] FAIL -- %s" % e)
        return 1

    dur_ms = 1000.0 * len(mono) / rate if rate else 0.0
    peak = max((abs(v) for v in mono), default=0)
    rms = math.sqrt(sum(v * v for v in mono) / len(mono)) if mono else 0.0
    # "Audible" = above a floor that ordinary dither/noise cannot reach.
    loud = sum(1 for v in mono if abs(v) > 512)
    print("[WAVCHK] duration=%.1f ms  frames=%d  peak=%d  rms=%.1f  "
          "loud_frames=%d (%.1f%%)"
          % (dur_ms, len(mono), peak, rms, loud,
             100.0 * loud / len(mono) if mono else 0.0))

    if peak == 0:
        print("[WAVCHK] VERDICT: SILENT -- capture is all zeroes")
        return 1
    if dur_ms < min_ms:
        print("[WAVCHK] VERDICT: TOO SHORT -- %.1f ms < %.1f ms"
              % (dur_ms, min_ms))
        return 1

    # Analyze only the loud span; the capture is mostly silence around a
    # short tone, and leading zeros would drag any frequency estimate down.
    first = next((i for i, v in enumerate(mono) if abs(v) > 512), 0)
    last = len(mono) - next((i for i, v in enumerate(reversed(mono))
                             if abs(v) > 512), 0)
    span = mono[first:last]
    print("[WAVCHK] loud span: frames %d..%d (%.1f ms)"
          % (first, last, 1000.0 * len(span) / rate))

    # Cap the analysis window: Goertzel is O(n) per bin and the sweep is
    # ~350 bins, so a multi-second capture would take minutes.
    if len(span) > rate:
        span = span[:rate]

    if span:
        # Zero-crossing is the primary estimate (property 3 in the header);
        # the Goertzel sweep is kept as a cross-check, since the two
        # disagreeing is itself a signal that the capture is not a clean tone.
        fz, nz = zc_freq(span, rate)
        fg, mag = dominant_freq(span, rate)
        print("[WAVCHK] frequency: %.1f Hz (zero-crossing, %d crossings)"
              % (fz, nz))
        print("[WAVCHK] frequency: %.1f Hz (goertzel sweep, mag %.1f)"
              % (fg, mag))
        f = fz if nz >= 3 else fg
        if expect_hz is not None:
            err = abs(f - expect_hz) / expect_hz
            ok = err <= tol
            print("[WAVCHK] expected %.1f Hz -- error %.2f%% (tol %.1f%%) %s"
                  % (expect_hz, 100.0 * err, 100.0 * tol,
                     "OK" if ok else "OUT OF TOLERANCE"))
            if not ok:
                print("[WAVCHK] VERDICT: FAIL -- wrong tone")
                return 1

    print("[WAVCHK] VERDICT: PASS -- real audio reached the backend")
    return 0


if __name__ == "__main__":
    sys.exit(main())
