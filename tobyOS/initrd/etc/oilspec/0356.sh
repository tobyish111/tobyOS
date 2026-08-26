case $SH in mksh|bash) exit ;; esac

a=(1 2 3)
var b = a
a+=(4 5)
echo "a=(${a[*]})"
echo "b=(${b[*]})"
