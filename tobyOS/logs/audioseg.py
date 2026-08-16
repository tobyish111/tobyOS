#!/usr/bin/env python3
"""audioseg.py -- sliding-window frequency profile of a QEMU wav capture.

Why this exists: a single boot emits more than one tone (the 880 Hz HDA
diagnostic, then the 1000 Hz playback test), and QEMU ELIDES the idle time
between them -- the streams land back-to-back in the file with no silence
to split on. Segmenting by amplitude therefore sees one long burst.

So segment by FREQUENCY instead: measure each window independently and
group adjacent windows that agree. The output is a timeline like

    0.000 - 0.730 s   ~877 Hz   (63 windows)
    0.730 - 1.340 s  ~1000 Hz   (52 windows)

which reads directly as "the diagnostic tone played, then userland audio
played" -- the exact claim slice 2 has to support.

Usage: audioseg.py <file.wav> [window_ms]
"""
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0].rsplit("/", 1)[0])
from wavcheck import parse_wav, to_mono, zc_freq   # noqa: E402


def main():
    if len(sys.argv) < 2:
        print("usage: audioseg.py <file.wav> [window_ms]")
        return 2
    path = sys.argv[1]
    win_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 25.0

    fmt, pcm = parse_wav(path)
    mono = to_mono(pcm, fmt["channels"], fmt["bits"])
    rate = fmt["rate"]
    win = max(1, int(rate * win_ms / 1000.0))

    rows = []
    for start in range(0, len(mono) - win + 1, win):
        seg = mono[start:start + win]
        peak = max(abs(v) for v in seg)
        if peak < 512:
            rows.append((start, 0.0))       # silence
            continue
        f, n = zc_freq(seg, rate)
        rows.append((start, f if n >= 3 else 0.0))

    # Group adjacent windows whose frequency agrees within 6%.
    groups = []
    for start, f in rows:
        if groups:
            gf = groups[-1]["sum"] / groups[-1]["n"]
            if (f == 0.0 and gf == 0.0) or (
                    f > 0 and gf > 0 and abs(f - gf) / gf < 0.06):
                groups[-1]["sum"] += f
                groups[-1]["n"] += 1
                groups[-1]["end"] = start + win
                continue
        groups.append({"start": start, "end": start + win,
                       "sum": f, "n": 1})

    print("windows of %.0f ms over %.3f s of capture:"
          % (win_ms, len(mono) / float(rate)))
    for g in groups:
        if g["n"] < 2:
            continue                        # ignore single-window flickers
        avg = g["sum"] / g["n"]
        label = "SILENCE" if avg == 0.0 else "~%.0f Hz" % avg
        print("  %6.3f - %6.3f s   %-9s (%d windows)"
              % (g["start"] / float(rate), g["end"] / float(rate),
                 label, g["n"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
