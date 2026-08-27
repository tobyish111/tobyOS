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
# An EMPTY QUOTED element is a real element -- a=("" "") is two, not zero.
# Regression caught by oils array-sparse when the blank-line skip ate these.
q=("" "")
echo "empty-quoted n=${#q[@]} 0=[${q[0]}] 1=[${q[1]}]"
r=(a "" b)
echo "mixed n=${#r[@]} 1=[${r[1]}] 2=[${r[2]}]"
echo end
