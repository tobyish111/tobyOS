# different than the case above because because 4 isn't the likely first FD

exec 4> "$TMP/fd4.txt"
echo hello >&4
echo world >&4
exec 4>&-  # close
cat "$TMP/fd4.txt"
