true 9> "$TMP/fd.txt"
( echo world >&9 )
cat "$TMP/fd.txt"
