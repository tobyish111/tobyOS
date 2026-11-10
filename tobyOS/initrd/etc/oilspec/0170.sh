case $SH in zsh|ash) exit ;; esac

a=(0 1 2)
b=(3 4 5)
typeset -p b

expr='a[$(echo 2)]' 

echo 'get' "${b[expr]}"

b[expr]=zzz

echo 'set' "${b[expr]}"
typeset -p b
