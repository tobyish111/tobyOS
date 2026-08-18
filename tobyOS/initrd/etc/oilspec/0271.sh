case $SH in mksh) exit ;; esac

a=({1..9})
unset -v 'a[2]' 'a[3]' 'a[7]'

echo $((a[-10]))
