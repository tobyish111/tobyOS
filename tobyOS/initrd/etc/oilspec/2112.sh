exec 3> "$TMP/fd.txt"
echo hello 3>&- << EOF
EOF
echo world >&3
exec 3>&-  # close
cat "$TMP/fd.txt"
