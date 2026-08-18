a=(1 2 3 4 5 6 7 8 9)
typeset -p a
unset -v "a[1]"
typeset -p a
unset -v "a[9]"
typeset -p a
unset -v "a[0]"
typeset -p a
