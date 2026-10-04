exec 2>&1 </dev/null
# q46's first line eight times: where the notice of the job a plain `wait`
# reaped lands against the next lines' output (bash varies run to run?)
i=0
while [ $i -lt 8 ]; do
  i=$((i+1))
  $SH -c 'set -m; echo "1 $-"; (echo "2 $-"); echo "3 $(echo $-)"; { echo "4 $-"; } | cat; (echo "5 $-") & wait; (set -m; echo "6 $-"); echo "7 $-"' 2>&1 | tr '\n' '|'
  echo
done | sort | uniq -c
