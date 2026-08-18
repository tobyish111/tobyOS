rm -f "$TMP/f.pipe"
mkfifo "$TMP/f.pipe"
exec 8<> "$TMP/f.pipe"
echo first >&8
echo second >&8
read line1 <&8
read line2 <&8
exec 8<&-
echo line1=$line1 line2=$line2
