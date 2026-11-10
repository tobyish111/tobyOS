exec 20> "$TMP/double-digit-fd.txt"
echo hello20 >&20
cat "$TMP/double-digit-fd.txt"
