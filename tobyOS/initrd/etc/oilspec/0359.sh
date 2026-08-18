case $SH in mksh) exit ;; esac

a=(x)
a[9]=y
echo "len ${#a[@]};"

unset -v 'a[-1]'
echo "len ${#a[@]};"
echo "last ${a[@]: -1};"
