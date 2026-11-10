case $SH in mksh) exit ;; esac

a=(1)
unset -v 'a[-2]'
