case $SH in mksh) exit ;; esac

a=(1 2 3 4)
echo "a=(${a[*]})"
echo "begin=-1 -> (${a[*]: -1})"
echo "begin=-2 -> (${a[*]: -2})"
echo "begin=-3 -> (${a[*]: -3})"
echo "begin=-4 -> (${a[*]: -4})"
echo "begin=-5 -> (${a[*]: -5})"
