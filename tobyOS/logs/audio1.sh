#!/bin/bash
# Audio slice 1: PROVE THE INSTRUMENT.
#
# Before connecting anything, establish that we can *observe* sound from a
# headless harness. QEMU's `wav` audiodev writes the emulated HDA codec's
# output to a real file -- the audio equivalent of a screendump. This script
# runs the ALREADY-WORKING tone selftest (audio_hda_tone_selftest, an 880 Hz
# square wave via BDL/DMA) and checks that the tone lands in the WAV.
#
# Known-good code + a new instrument = the instrument is on trial here, not
# the kernel. If this fails, the measurement is broken, not the driver.
#
# Two things had to be true and neither was:
#   1. The standard harness passes NO audio device at all, so hda_probe() has
#      never run in the dev loop. We add -device intel-hda -device hda-output.
#   2. QUICK_BOOT skips devtest_boot_run, which is the only caller of the
#      tone test. QUICK_BOOT is referenced ONLY in src/kernel.c (verified),
#      so dropping it costs one file's recompile, not a full rebuild.
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

WAV=logs/audio1.wav
WAV_WIN='C:/CustomOS/tobyOS/logs/audio1.wav'
LOG=logs/audio1.log
PORT=4455

# EXTRA_CFLAGS REPLACES the defaults, so FAST_BOOT must be re-stated; we drop
# only QUICK_BOOT. `make` keys off mtime, NOT the -D flags, so touch the one
# file that reads the macro or the old kernel.o is silently reused.
touch src/kernel.c
mkdir -p /c/t
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="-DFAST_BOOT" iso \
     >logs/audio1.build.log 2>&1; then
    echo "ISO BUILD FAIL -- tail:"; tail -25 logs/audio1.build.log; exit 1
fi
[ -f tobyOS.iso ] || { echo "ISO BUILD FAIL (no ISO)"; exit 1; }
if [ tobyOS.iso -ot tobyos.bin ]; then
    echo "ISO BUILD FAIL: tobyOS.iso is OLDER than tobyos.bin -- stale image"
    exit 1
fi
# Prove the flavour actually took, the way the TKAPP trap taught us: grep the
# BINARY for a string only the non-QUICK_BOOT path can emit.
if grep -qa "QUICK_BOOT: skipping devtest_boot_run" tobyos.bin; then
    echo "NOTE: kernel still carries the QUICK_BOOT message (string is compiled in)"
fi

QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
: > "$LOG"; rm -f "$WAV"

# -audiodev wav writes every sample the codec emits to $WAV.
# hda-output is the output-only codec (line-out) -- it presents the DAC+PIN
# widget pair the tone test looks for.
"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -audiodev wav,id=snd0,path="$WAV_WIN" \
  -device intel-hda -device hda-output,audiodev=snd0 \
  -smp 4 -m 4096 -cpu qemu64,+smep,+smap -serial "file:$LOG" \
  -qmp tcp:127.0.0.1:$PORT,server,nowait -no-reboot -display none &

# Wait for the devtest sweep to report, not for a wall-clock guess.
for i in $(seq 1 180); do
    grep -aq "M26A: .* test(s)" "$LOG" 2>/dev/null && break
    sleep 1
done
sleep 2

echo "=== HDA probe ==="
grep -aE "\[hda\]|\[audio" "$LOG" | head -20
echo
echo "=== audio devtests ==="
grep -aiE "M26A: (audio|audio_tone)" "$LOG" | head -10
echo
echo "=== devtest totals ==="
grep -a "M26A: .* test(s)" "$LOG" | tail -2
echo
echo "=== faults (empty=clean) ==="
grep -aiE 'PANIC|EXCEPTION [0-9]+:|#GP|#PF' "$LOG" | head -5

# Shut QEMU down CLEANLY via QMP so the wav backend patches its RIFF sizes.
# wavcheck.py survives an unfinalized header anyway, but a clean file keeps
# the artifact readable by ordinary tools.
if exec 3<>/dev/tcp/127.0.0.1/$PORT 2>/dev/null; then
    printf '%s\n' '{"execute":"qmp_capabilities"}' >&3
    sleep 1
    printf '%s\n' '{"execute":"quit"}' >&3
    sleep 2
    exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null
fi
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1
sleep 1

echo
echo "=== WAV capture ==="
if [ ! -f "$WAV" ]; then
    echo "NO WAV FILE -- the audiodev never opened. Instrument FAILED."
    exit 1
fi
ls -la "$WAV"
python logs/wavcheck.py "$WAV" --expect-hz 880 --min-ms 20
rc=$?

# Leave the tree on the STOCK flavour so the next build doesn't silently
# inherit this one's kernel.o (the slice-61f lesson).
touch src/kernel.c
echo
echo "NOTE: tobyOS.iso is the FAST_BOOT-only (no QUICK_BOOT) flavour."
exit $rc
