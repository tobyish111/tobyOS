# different than case below because 3 is the likely first FD of open()

exec 3> "$TMP/fd3.txt"
echo hello >&3
echo world >&3
exec 3>&-  # close
cat "$TMP/fd3.txt"
