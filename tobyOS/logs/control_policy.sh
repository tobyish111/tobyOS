#!/bin/bash
# Slice 72: bisect the environment difference at chrome's death point.
#
# The healthy-Linux diff shows the first thing chrome does after its last
# tobyOS-visible log line is POLICY LOADING, and Ubuntu prints "Skipping
# mandatory platform policies because no policy file" -- while tobyOS's
# initrd STAGES one (Makefile: etc/opt/chrome/policies/managed/tobyos.json,
# PostQuantumKeyAgreementEnabled=false, added for the ML-KEM wall).
# So: reproduce that file here and see whether the known-good chrome CHECKs.
set -u
CD=/mnt/c/CustomOS/tobyOS/programs/chromium/chrome-linux64
OUT=/mnt/c/CustomOS/tobyOS/logs/control
mkdir -p "$OUT"; cd "$CD" || exit 1

for pd in /etc/opt/chrome/policies/managed /etc/chromium/policies/managed \
          /etc/opt/chrome_for_testing/policies/managed; do
    mkdir -p "$pd"
    printf '{ "PostQuantumKeyAgreementEnabled": false }\n' > "$pd/tobyos.json"
done
echo "staged policy files exactly as the tobyOS initrd does"

rm -rf /tmp/cr_policy
timeout 60 ./chrome --headless=new --no-sandbox --no-zygote \
    --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
    --in-process-gpu --disable-kill-after-bad-ipc --disable-dev-shm-usage \
    --disable-crash-reporter --disable-in-process-stack-traces --no-first-run \
    --enable-logging=stderr --v=1 --user-data-dir=/tmp/cr_policy \
    --window-size=800,600 --mute-audio --disable-audio-output \
    --dump-dom about:blank >"$OUT/out_policy.txt" 2>"$OUT/err_policy.txt"
echo "exit=$? stderr=$(wc -l <"$OUT/err_policy.txt") lines"
echo "=== policy lines:"
grep -aiE 'policy' "$OUT/err_policy.txt" | head -4 | cut -c1-150
echo "=== CHECK/FATAL:"
grep -aiE 'check failed|FATAL|received signal' "$OUT/err_policy.txt" | head -3 | cut -c1-150
echo "(nothing above = the policy file is NOT the trigger)"

# Clean up so the host distro is left as we found it.
rm -f /etc/opt/chrome/policies/managed/tobyos.json \
      /etc/chromium/policies/managed/tobyos.json \
      /etc/opt/chrome_for_testing/policies/managed/tobyos.json
