case $SH in bash|mksh) exit ;; esac

a=(1 2 3)
unset -v 'a[1]'
append 'x' 'y' 'z' (a)
= a
