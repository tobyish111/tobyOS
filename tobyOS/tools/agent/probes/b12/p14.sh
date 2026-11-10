exec 2>&1
a=(1 2); echo "1 ${a[]}"; echo s=$?
echo "2 ${a[]:-x}"
echo 3 ${a[]}
x=${a[]}; echo "4 [$x] $?"
echo "5 ${#a[]}"
echo "6 ${!a[]}"
echo "7 ${a[]#x}"
