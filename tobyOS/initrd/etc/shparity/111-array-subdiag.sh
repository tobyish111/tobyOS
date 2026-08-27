# One subscript form per line: a form that fails kills only its own line, so
# the diff names the culprit instead of blanking the whole case.
a=(zero one two three)
i=1
echo "A=${a[1]}"
echo "B=${a[i]}"
echo "C=${a[$i]}"
echo "D=${a[i+1]}"
echo "E=${a[-1]}"
echo "F=${a[@]:1}"
echo "G=${a[@]:1:2}"
echo end
