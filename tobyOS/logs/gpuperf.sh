#!/bin/bash
# TIER 3 PHASE 1d (slice 106): THE MEASURE-FIRST GATE.
#
# Tier 3 was designed with an explicit condition attached: GPU raster has to
# prove it beats the CPU-raster baseline before the rest of the surface gets
# built. Slice 105 finished the plumbing (GL_RENDERER: virgl), so this script
# is the A/B that answers it.
#
#   bash logs/gpuperf.sh cpu   anim     baseline: --use-gl=disabled
#   bash logs/gpuperf.sh gl    anim     GPU raster through Mesa/virgl
#   bash logs/gpuperf.sh cpu   webgl    expect ctx=NONE (capability control)
#   bash logs/gpuperf.sh gl    webgl    the raster-BOUND workload
#
# Two gates, both non-negotiable, both paid for by earlier slices:
#   1. '[bkl] cpu' must appear in the log. Empty = the guest froze and the run
#      is evidence about nothing (standing rule for this whole arc).
#   2. In gl mode the tobygl renderer string must name virgl. Chrome falling
#      back to SwiftShader yields a perfectly believable fps number that
#      measures the CPU path -- a VACUOUS result, which is precisely the trap
#      slice 91 fell into by trusting an unvalidated control.
set -u
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t
export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'
cd /c/CustomOS/tobyOS || exit 1

MODE="${1:-cpu}"          # cpu | gl (ANGLE-on-GL) | gle (native EGL, no ANGLE)
PAGE="${2:-anim}"         # anim | webgl
TAG="${MODE}_${PAGE}"
PY=/c/Users/tdude/AppData/Local/Programs/Python/Python311/python

case "$PAGE" in
  anim)  URL='file:///etc/anim.html' ;;
  webgl) URL='file:///etc/webgl.html' ;;
  *)     echo "usage: $0 {cpu|gl} {anim|webgl}"; exit 2 ;;
esac

# The quotes around the URL must reach the COMPILER, which means surviving two
# shells: this one, and the one make runs the recipe in. Single-quoting the
# backslash-quote is the only form that does -- "-DCW_URL=\"$URL\"" collapses to
# bare quotes here, make's shell then strips them, and clang sees
#   #define CW_URL file:///etc/anim.html
# which fails as an undeclared identifier `file`. (Cost one build.)
PROGF='-DCW_URL=\"'"$URL"'\"'
case "$MODE" in
  gl)  PROGF="$PROGF -DCW_GL" ;;                    # ANGLE-on-GL (needs X11)
  gle) PROGF="$PROGF -DCW_GL -DCW_GL_NATIVE" ;;     # chromium native EGL (REFUSED by this build)
  gld) PROGF="$PROGF -DCW_GL -DCW_GL_DRM" ;;        # ANGLE + Ozone DRM/GBM
  cpu) ;;
  *)   echo "usage: $0 {cpu|gl|gle|gld} {anim|webgl}"; exit 2 ;;
esac

echo "=== [1/3] build: mode=$MODE page=$PAGE ==="
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1
sleep 1                     # let Windows release the ISO lock (slice 61f)
rm -f build/initrd.tar build/base.iso tobyOS.iso
# chromewin.o does not depend on these defines, so make would happily keep a
# stale object built for the OTHER arm of the A/B (slice 73's lesson, and it
# would silently invert the experiment here).
rm -f programs/chromewin/chromewin.o programs/chromewin/chromewin.elf
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT -DTKAPP_BOOT -DTKAPP_CHROMEWIN" \
     PROG_EXTRA_CFLAGS="$PROGF" > "logs/gpuperf_${TAG}.build.log" 2>&1; then
    echo "BUILD FAILED -- tail:"; tail -25 "logs/gpuperf_${TAG}.build.log"; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAILED (no ISO)"; exit 1; }
grep -a "CW_GL\|chromewin.elf" "logs/gpuperf_${TAG}.build.log" | tail -2
ls -l --time-style=full-iso tobyOS.iso

echo "=== [2/3] run: SMP=4 $( [ "$MODE" != cpu ] && echo 'GLDEV=1 (virtio-gpu-gl-pci + ANGLE)' ) ==="
if [ "$MODE" != "cpu" ]; then
    [ -d logs/angle ] || bash logs/setup_angle.sh > "logs/gpuperf_${TAG}.angle.log" 2>&1
    SMP=4 GLDEV=1 $PY logs/run_watch.py 2>&1 | tail -6
else
    SMP=4 $PY logs/run_watch.py 2>&1 | tail -6
fi
cp logs/run_watch.log "logs/gpuperf_${TAG}.log"
L="logs/gpuperf_${TAG}.log"

echo "=== [3/3] verdict ==="
# GATE 1 -- liveness, measured as GUEST CLOCK PROGRESS.
#
# The standing rule for this arc is "'[bkl] cpu' must be non-empty or the run
# is evidence about nothing". That report only fires on a 60-SECOND interval,
# so on a short diagnostic run its absence means nothing -- and it duly cried
# freeze on a 90s run that had merely not reached the first emission. Worse,
# it cannot see the failure that actually happened there: the guest did not
# stop, it CRAWLED (7.3s of guest clock in 90s of wall time, a chrome thread
# storming ~4GB PROT_NONE mmap/munmap probes).
#
# So gate on the thing both failures share: did the guest clock keep up with
# the wall clock? That catches a freeze, a crawl, and a panic alike, at any
# run length. '[bkl] cpu' stays as an extra check where it is meaningful.
RUNMS=$(( ${RUNSECS:-360} * 1000 ))
MAXTS=$(python - "$L" <<'PY'
import io,re,sys
d=io.open(sys.argv[1],encoding="utf-8",errors="replace").read()
ts=[int(m) for m in re.findall(r"\[(\d+) ms\]",d)]
print(max(ts) if ts else 0)
PY
)
# Boot eats a few seconds and the runner tears down at TOTAL, so anything past
# half the wall clock means the guest was keeping pace.
MINTS=$(( RUNMS / 2 ))
echo "gate1 liveness: guest clock reached ${MAXTS}ms of ${RUNMS}ms wall"
if [ "${MAXTS:-0}" -lt "$MINTS" ]; then
    echo "GATE FAIL: the guest did not keep up with the wall clock (< ${MINTS}ms)."
    echo "           It froze or crawled; this run is evidence about nothing."
    echo "           last guest activity:"
    tail -c 400 "$L" | tr -d '\r' | tail -3
    exit 1
fi
BKL=$(grep -ac '\[bkl\] cpu' "$L")
if [ "$RUNMS" -ge 300000 ] && [ "$BKL" -eq 0 ]; then
    echo "GATE FAIL: full-length run with no '[bkl] cpu' report."
    exit 1
fi
echo "gate1 liveness: OK (bkl reports: $BKL)"

# GATE 2 -- is it really the GPU? (gl mode only)
GLLINE=$(grep -ao 'tobygl [^"\\]*' "$L" | tail -1)
echo "renderer: ${GLLINE:-<no tobygl reply -- probe never answered>}"
if [ "$MODE" != "cpu" ]; then
    if echo "$GLLINE" | grep -qi 'virgl'; then
        echo "gate2 GPU: OK (chrome names virgl)"
    else
        echo "gate2 GPU: *** VACUOUS *** -- chrome is NOT on virgl, so any fps"
        echo "           below measures the CPU path with extra steps."
        echo "           chrome's own GPU narration:"
        grep -aiE 'gpu_init|GLSurface|gl_display|Passthrough|SwiftShader|swrast|fallback|GPU process' \
            "$L" | tail -12
    fi
fi

FRAMES=$(grep -ao 'exiting; frames=[0-9]*' "$L" | tail -1 | grep -o '[0-9]*')
[ -z "$FRAMES" ] && FRAMES=$(grep -ao 'frames=[0-9]*' "$L" | tail -1 | grep -o '[0-9]*')
echo "FRAMES($TAG) = ${FRAMES:-none}   (360s run => fps = frames/360)"
grep -ao 'probe #[0-9]* at [0-9]*s: frames=[0-9]*' "$L" | tail -3
echo "--- [cwif] decomposition (last) ---"
grep -a '\[cwif\]' "$L" | tail -2
echo "--- faults (empty=clean) ---"
grep -aiE 'KERNEL PANIC|EXCEPTION [0-9]+|#GP|terminating user process' "$L" \
  | grep -viE 'reset_reg' | head -5
echo "--- [drm] unhandled (the gap list, empty=clean) ---"
grep -a '\[drm\] UNHANDLED' "$L" | sort | uniq -c | head
echo "(end $TAG)"
