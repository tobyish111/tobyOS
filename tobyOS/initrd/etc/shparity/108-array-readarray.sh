# readarray/mapfile fill an array from lines on stdin.
printf 'l1\nl2\nl3\n' > /tmp/ra.txt
readarray -t arr < /tmp/ra.txt
echo "n=${#arr[@]} 0=${arr[0]} 2=${arr[2]}"
mapfile -t arr2 < /tmp/ra.txt
echo "m n=${#arr2[@]} 1=${arr2[1]}"
readarray arr3 < /tmp/ra.txt
echo "keepnl=${#arr3[@]} [${arr3[0]}]"
rm -f /tmp/ra.txt
echo end
