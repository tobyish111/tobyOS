case $SH in bash|mksh) exit ;; esac

a1=()
a2=(0)
a3=(0 1 2)
a4=(0 0)
unset -v 'a4[0]'

shopt -s ysh:upgrade

echo $[bool(a1)]
echo $[bool(a2)]
echo $[bool(a3)]
echo $[bool(a4)]
