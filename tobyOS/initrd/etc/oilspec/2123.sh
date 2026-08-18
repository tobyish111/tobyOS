exec 5> "$TMP/f.txt"
echo hello5 >&5
exec 6>&5-
echo world5 >&5
echo world6 >&6
exec 6>&-
cat "$TMP/f.txt"
