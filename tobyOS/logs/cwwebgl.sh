#!/bin/bash
# cwwebgl.sh -- does chrome have a WebGL context AT ALL? The gate for the
# slice-123 ANGLE-on-SwiftShader flavour (-DCW_SWGL).
#
# WHY THIS EXISTS: every software build reports `tobygl ctx=NONE` -- not slow
# WebGL, NO WebGL. A Chrome 151 that cannot create a WebGL context is a
# fingerprint anomaly visible to any page for free, and it is one of the few
# real differences between this browser and a normal one (see the bot-gate
# history; the standing rule there is do not SPOOF, so the answer is to
# actually have the capability).
#
# THE VERDICT IS THE RENDERER STRING, NOT THE FLAGS. Chrome accepts
# --use-angle=swiftshader and can still land on nothing: a missing
# --enable-unsafe-swiftshader fails EXACTLY like a build with no SwiftShader
# at all (ctx=NONE, no error printed). So this asserts on what the PAGE sees
# via gl_renderer_probe()'s tobygl line, and additionally requires the page
# to still render -- a browser that gains WebGL and loses painting is a
# regression, not a feature.
#
#   PASS -> tobygl reports a context whose renderer names SwiftShader,
#           AND frames are still produced, AND no [net] FAILED
#   FAIL -> ctx=NONE (the flavour did not take) or rendering regressed
cd /c/CustomOS/tobyOS || exit 1
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
mkdir -p /c/t; export TMP='C:\t' TEMP='C:\t' TMPDIR='C:\t'

URL="${URL:-https://www.bing.com/search?q=webgl+test}"
TAG="${TAG:-cwwebgl}"
export RUNSECS="${RUNSECS:-220}"
export SMP="${SMP:-4}"

PROGF='-DCW_SWGL -DCW_URL=\"'"$URL"'\"'
KCF="-DFAST_BOOT -DQUICK_BOOT -DCHROMIUM_BOOT -DTKAPP_BOOT -DTKAPP_CHROMEWIN"

echo "=== [1/3] build: chromewin -DCW_SWGL -> $URL ==="
taskkill //F //IM qemu-system-x86_64.exe >/dev/null 2>&1; sleep 1
rm -f src/*.o
rm -f programs/chromewin/chromewin.o programs/chromewin/chromewin.elf
rm -f build/initrd.tar build/base.iso tobyOS.iso
if ! make "CC=TMP='C:\\t' TEMP='C:\\t' clang" \
          "HOST_CC=TMP='C:\\t' TEMP='C:\\t' gcc" iso \
     EXTRA_CFLAGS="$KCF" PROG_EXTRA_CFLAGS="$PROGF" \
     > "logs/${TAG}.build.log" 2>&1; then
    echo "BUILD FAILED -- tail:"; tail -25 "logs/${TAG}.build.log"; exit 1
fi
[ -f tobyOS.iso ] || { echo "BUILD FAILED (no ISO)"; exit 1; }

# Gate on the BINARY: PROG_EXTRA_CFLAGS reaching chromewin has been a real
# trap in this tree (EXTRA_CFLAGS silently does not), and a stale object
# would produce a confident ctx=NONE that means nothing.
grep -qa -- "use-angle=swiftshader" programs/chromewin/chromewin.elf \
    || { echo "  FAIL: CW_SWGL flags not in chromewin.elf -- stale object"; exit 1; }
grep -qa -- "enable-unsafe-swiftshader" programs/chromewin/chromewin.elf \
    || { echo "  FAIL: --enable-unsafe-swiftshader missing"; exit 1; }
echo "  ok: chromewin.elf carries the SwiftShader flag set"

echo "=== [2/3] run ==="
python logs/run_watch.py 2>&1 | tail -3
L="logs/${TAG}.log"
cp logs/run_watch.log "$L"

echo
echo "=== [3/3] VERDICT ==="
echo "--- what the PAGE sees ---"
tr '\r' '\n' < "$L" | grep -ao "tobygl[^\"]*" | tail -3
echo "--- chrome's own GL narration ---"
tr '\r' '\n' < "$L" | grep -aoE "SwiftShader|ANGLE [^\"]{0,60}|GLImplementation[^\"]{0,40}" | sort -u | head -6
echo "--- frames (rendering must NOT regress) ---"
tr '\r' '\n' < "$L" | grep -ao "frame [0-9]*: [0-9]*x[0-9]* jpeg=[0-9]*" | tail -2
echo

# Strip NULs before every count: the serial capture carries them, and a
# pipeline that forgets flips grep into binary mode on some hosts.
CLEAN=$(mktemp); tr -d '\000' < "$L" | tr '\r' '\n' > "$CLEAN"

ctx_none=$(grep -ac "tobygl ctx=NONE" "$CLEAN")
sw=$(grep -ac "tobygl.*[Ss]wift[Ss]hader" "$CLEAN")
frames=$(grep -ac "frame [0-9]*: " "$CLEAN")
# net::ERR_ABORTED is chrome CANCELLING ITS OWN speculative/prefetch loads.
# It appears on healthy runs (the known-good Bing render had it too), so
# counting it as failure made the first version of this gate report
# INCONCLUSIVE on a run that had in fact PASSED -- WebGL live, 10 frames,
# nothing wrong. Count only failures that are not self-inflicted aborts.
nfail=$(grep -a -A3 "\[net\] FAILED" "$CLEAN" | grep -aoE "net::[A-Z_]+" \
        | grep -av "ERR_ABORTED" | wc -l)
naborted=$(grep -a -A3 "\[net\] FAILED" "$CLEAN" | grep -aoc "ERR_ABORTED")
rm -f "$CLEAN"
echo "signals: webgl=$sw ctx_none=$ctx_none frames=$frames real_net_fail=$nfail (aborted=$naborted, benign)"
echo

# Every combination lands somewhere: no silent fallthrough. The first
# version had no branch for "WebGL + frames + some net failures" and quietly
# called a passing run inconclusive.
if [ "$sw" = "0" ] && [ "$ctx_none" != "0" ]; then
    echo "FAIL: still ctx=NONE -- the flavour did not take (flags accepted,"
    echo "  context still refused; check chrome's GL narration above)"
elif [ "$sw" = "0" ]; then
    echo "INCONCLUSIVE: no tobygl verdict at all -- did the probe run? did"
    echo "  chrome reach a page? check $L"
elif [ "$frames" = "0" ]; then
    echo "FAIL: WebGL appeared but RENDERING STOPPED -- worse than no WebGL"
elif [ "$nfail" != "0" ]; then
    echo "FAIL: WebGL live and rendering, but $nfail non-abort network failure(s)"
else
    echo "PASS: WebGL context present (SwiftShader), page still renders"
fi
echo "=== log: $L ==="
