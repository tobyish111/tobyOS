#!/bin/bash
# Audio slice 2: PROVE THE JUNCTION.
#
# Slice 1 proved we can observe sound (the tone diagnostic reached the WAV).
# This proves the path that actually matters: bytes written by a userland
# program with sys_audio_write() come out of the DAC.
#
# That path did not exist before this slice. audio_engine.c mixed into
# g_audio_dma_ring; the only reader of that ring, audio_engine_get_dma_data(),
# had ZERO callers in the whole tree. The mixer and the driver were written
# to meet in the middle and never did.
#
# /bin/audioplay emits a 1000 Hz square wave -- exactly 48 frames per period
# at 48 kHz, so unlike the 880 Hz tone diagnostic (1024 frames holding 18.96
# periods, which reads ~2% low) it has no wrap discontinuity and can be
# measured tightly.
#
# FULL REBUILD IS MANDATORY HERE: this slice changes the VALUE of
# FILE_KIND_DEVRANDOM in file.h, and the Makefile has no header dependency
# tracking, so a partial build would leave some objects on the old numbering
# and cross-wire device kinds silently.
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"

WAV=logs/audio3.wav
WAV_WIN='C:/CustomOS/tobyOS/logs/audio3.wav'
LOG=logs/audio3.log
PORT=4457

mkdir -p /c/t
if [ "$1" = "--full" ]; then
    echo "=== full rebuild (FILE_KIND enum changed) ==="
    rm -f src/*.o
fi
touch src/kernel.c
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" \
          EXTRA_CFLAGS="-DFAST_BOOT" iso \
     >logs/audio3.build.log 2>&1; then
    echo "ISO BUILD FAIL -- tail:"; tail -30 logs/audio3.build.log; exit 1
fi
[ -f tobyOS.iso ] || { echo "ISO BUILD FAIL (no ISO)"; exit 1; }
if [ tobyOS.iso -ot tobyos.bin ]; then
    echo "ISO BUILD FAIL: tobyOS.iso is OLDER than tobyos.bin -- stale"; exit 1
fi

QEMU="/c/Program Files/qemu/qemu-system-x86_64.exe"
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1; sleep 1
: > "$LOG"; rm -f "$WAV"

"$QEMU" -cdrom tobyOS.iso \
  -drive file=disk.img,format=raw,if=ide,index=0,media=disk,cache=writethrough -boot d \
  -netdev user,id=net0 -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -audiodev wav,id=snd0,path="$WAV_WIN" \
  -device intel-hda -device hda-output,audiodev=snd0 \
  -smp 4 -m 4096 -cpu qemu64,+smep,+smap -serial "file:$LOG" \
  -qmp tcp:127.0.0.1:$PORT,server,nowait -no-reboot -display none &

for i in $(seq 1 240); do
    grep -aq "M26A: userland" "$LOG" 2>/dev/null && break
    sleep 1
done
sleep 3

echo "=== hda pcm stream ==="
grep -aE 'hda\] pcm|audio_engine|audio\] stream' "$LOG" | head -12
echo
echo "=== ALSA device (slice 3) ==="
grep -aE "LXSND|snd. hw_params|linux-sndtest" "$LOG" | head -25
echo
echo "=== verdict ==="
grep -aE "LXSND. VERDICT|M26A: userland" "$LOG" | tail -2
echo
echo "=== faults (empty=clean) ==="
grep -aiE 'PANIC|EXCEPTION [0-9]+:|#GP|#PF' "$LOG" \
  | grep -viE 'M28A_TAG|panic-test|SetUnhandledException' | head -5

if exec 3<>/dev/tcp/127.0.0.1/$PORT 2>/dev/null; then
    printf '%s\n' '{"execute":"qmp_capabilities"}' >&3; sleep 1
    printf '%s\n' '{"execute":"quit"}' >&3; sleep 2
    exec 3<&- 2>/dev/null; exec 3>&- 2>/dev/null
fi
taskkill //IM qemu-system-x86_64.exe //F >/dev/null 2>&1
sleep 1

echo
echo "=== WAV capture ==="
[ -f "$WAV" ] || { echo "NO WAV FILE -- audiodev never opened"; exit 1; }
ls -la "$WAV"
# The capture also contains the 880 Hz tone diagnostic (it runs earlier in
# the same boot), so check the WHOLE file loosely and then look for the
# 1000 Hz span specifically.
python logs/wavcheck.py "$WAV" --min-ms 100
rc=$?
echo
echo "=== per-burst breakdown (880 = tone diagnostic, 1000 = audioplay) ==="
python logs/audioseg.py "$WAV"
touch src/kernel.c
exit $rc
