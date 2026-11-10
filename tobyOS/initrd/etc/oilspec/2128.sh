exec 5>$TMP/log.txt
echo hi >&5
set -o >&5
echo done
