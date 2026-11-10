exec 5> "$TMP/f.txt"
echo hello >&5
exec 5>&-
echo world >&5
cat "$TMP/f.txt"
