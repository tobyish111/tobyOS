exec 2>&1 </dev/null
printf 'a\r\n' | $SH -c 'read x; echo "rc=$? len=${#x}"'
printf 'a\r\n' | $SH -c 'read -r x; echo "len=${#x}"'
printf 'ab\n' | $SH -c 'read x; echo "len=${#x}"'
printf 'a\rb\n' | $SH -c 'IFS= read x; echo "len=${#x}"'
printf 'a\r\n' | $SH -c 'read x y; echo "len=${#x} ${#y}"'
printf 'a\r\n' > /tmp/q56.in; $SH -c 'read x < /tmp/q56.in; echo "len=${#x}"'
$SH -c 'x=$(printf "a\r"); echo "len=${#x}"'
rm -f /tmp/q56.in
echo end
