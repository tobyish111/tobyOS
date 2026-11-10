exec 2>&1
b=()
echo "1 ${b[@]/x/$(echo SIDE1 >&2)} c"
echo "2 ${b[@]#$(echo SIDE2 >&2)} c"
echo "3 ${b[@]:$(echo SIDE3 >&2; echo 0)} c"
echo "4 ${b/x/$(echo SIDE4 >&2)} c"
declare -A B=()
echo "5 ${B[@]/x/$(echo SIDE5 >&2)} c"
echo "6 ${u@Z} c"
echo "7 after"
