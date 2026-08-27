# Indexed arrays: literal assignment, element read, whole-array expansion.
# Every case here stays inside builtins so it measures the SHELL.
a=(one two three)
echo "0=${a[0]} 1=${a[1]} 2=${a[2]}"
echo "bare=$a"
echo "star=${a[*]}"
echo "at=${a[@]}"
echo "count=${#a[@]} altcount=${#a[*]}"
echo "len1=${#a[1]}"
echo "unset-elem=[${a[9]}]"
b=()
echo "empty-count=${#b[@]} empty=[${b[@]}]"
c=(solo)
echo "solo=${c[0]} n=${#c[@]}"
echo end
