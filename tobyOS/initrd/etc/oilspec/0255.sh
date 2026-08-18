case $SH in bash|mksh) exit ;; esac

shopt -s ysh:upgrade

#pp test_ (a)

sp=( foo {25..27} bar )

sp[10]='sparse'

echo $[type(sp)]

echo len: "${#sp[@]}"

#echo $[len(sp)]

echo subst: "${sp[@]}"
echo keys: "${!sp[@]}"

echo slice: "${sp[@]:2:3}"

sp[0]=set0

echo get0: "${sp[0]}"
echo get1: "${sp[1]}"
echo ---

to_append=(x y)
echo append
sp+=("${to_append[@]}")
echo subst: "${sp[@]}"
echo keys: "${!sp[@]}"
echo ---

echo unset
unset -v 'sp[11]'
echo subst: "${sp[@]}"
echo keys: "${!sp[@]}"
