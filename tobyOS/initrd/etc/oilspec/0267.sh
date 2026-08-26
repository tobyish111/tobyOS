case $SH in mksh) exit ;; esac

a=({1..9})
unset -v 'a[-1]'
a[-1]=x
declare -p a
unset -v 'a[-1]'
a[-1]=x
declare -p a
