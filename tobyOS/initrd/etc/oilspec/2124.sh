# 7/2021: descriptor 8 is open on Github Actions, so use descriptor 6 instead

# Fix for CI systems where process state isn't clean: Close descriptors 6 and 7.
exec 6>&- 7>&-

opened=$(ls /proc/$$/fd)
if echo "$opened" | egrep '^7$'; then
  echo "FD 7 shouldn't be open"
  echo "OPENED:"
  echo "$opened"
fi
if echo "$opened" | egrep '^6$'; then
  echo "FD 6 shouldn't be open"
  echo "OPENED:"
  echo "$opened"
fi

exec 7> "$TMP/f.txt"
: 6>&7 7>&-
echo hello >&7
: 6>&7-
echo world >&7
exec 7>&-
cat "$TMP/f.txt"
