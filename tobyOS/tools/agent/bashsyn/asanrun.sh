#!/bin/bash
# asanrun.sh REF WRAPDIR -- in WSL: prharness built with ASan+UBSan
# (prharness_asan, next to this script) over every REF/{corpus,fz1,fz2,fz3}
# input with every command printed (PR_ALL), and the declare -f path over
# WRAPDIR. Any sanitizer report is a failure.
cd "$(dirname "$0")" || exit 1
R=$1; W=$2
mkdir -p /tmp/pra
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 UBSAN_OPTIONS=print_stacktrace=1
: > /tmp/pra.log
for d in $R/corpus $R/fz1 $R/fz2 $R/fz3; do
  ls $d | grep '\.sh$' | sed "s|^|$d/|" | PR_ALL=1 xargs -n 300 ./prharness_asan /tmp/pra 2>> /tmp/pra.log
done
ls $W/*.sh | xargs -n 300 ./prharness_asan /tmp/pra 2>> /tmp/pra.log
echo "reports: $(grep -c 'ERROR: AddressSanitizer\|runtime error\|LeakSanitizer' /tmp/pra.log)"
grep -m5 'ERROR: AddressSanitizer\|runtime error\|LeakSanitizer' /tmp/pra.log
