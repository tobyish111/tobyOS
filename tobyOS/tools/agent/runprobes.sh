#!/bin/bash
# runprobes.sh TAG PROBE... -- probe3 each probe (bash vs the installed tsh)
# into ${OUTBASE:-logs/b16}/TAG/, one verdict line each, summary at the end.
cd /c/CustomOS/tobyOS || exit 1
export ED_HOST=${ED_HOST:-127.0.0.1} ED_PORT=${ED_PORT:-2222} ED_USER=${ED_USER:-root} ED_TIMEOUT=${ED_TIMEOUT:-600}
tag=$1; shift
mkdir -p ${OUTBASE:-logs/b16}/$tag
for p in "$@"; do
  d=$(basename "$(dirname "$p")")
  n=$(basename "$p" .sh)
  v=$(bash tools/agent/probe3.sh ${OUTBASE:-logs/b16}/$tag/$d-$n "$p" 2>&1 | tail -1)
  case "$v" in *NORESULT*) v=$(bash tools/agent/probe3.sh ${OUTBASE:-logs/b16}/$tag/$d-$n "$p" 2>&1 | tail -1);; esac
  echo "$d/$v"
done | tee ${OUTBASE:-logs/b16}/$tag/verdicts.txt
echo "SUMMARY $(grep -c IDENTICAL ${OUTBASE:-logs/b16}/$tag/verdicts.txt) identical, $(grep -c DIFFERENT ${OUTBASE:-logs/b16}/$tag/verdicts.txt) different, $(grep -c NORESULT ${OUTBASE:-logs/b16}/$tag/verdicts.txt) noresult"
