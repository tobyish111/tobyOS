case $SH in zsh|ash) exit ;; esac

a=(1 2 3)
a=99
typeset -p a

typeset -A A=([k]=v)
A=99
typeset -p A
