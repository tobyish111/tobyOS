# waitn2.sh -- b14/r5 whole, 10 times in a fresh $SH, and the `wait -n`
# lines it printed each time, counted (finding 74: bash answered 127 or 0
# run to run for the first). Needs tools/agent/probes/b14/r5.sh at
# /tmp/pr/r5.sh on the guest.
exec 2>&1 </dev/null
SH=${SH:-bash}
i=0; while [ $i -lt 10 ]; do i=$((i+1))
  $SH /tmp/pr/r5.sh 2>&1 | grep '^wait -n' | tr '\n' ' '; echo
done | sort | uniq -c | sed 's/^ *//'
echo end
