case $SH in bash|mksh) exit ;; esac

a1=(1 2 3)
unset -v 'a1[1]'
a2=(1 2 3)
unset -v 'a2[1]'
a3=(1 2 4)
unset -v 'a3[1]'
a4=(1 2 3)

shopt -s ysh:upgrade

echo $[a1 === a1]
echo $[a1 === a2]
echo $[a1 === a3]
echo $[a1 === a4]
echo $[a2 === a1]
echo $[a3 === a1]
echo $[a4 === a1]
