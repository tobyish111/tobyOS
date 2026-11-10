case $SH in zsh|ash) exit ;; esac

a[1 + 2]=7
a[3|4]=8
a[(1+2)*3]=9

typeset -p a

# Dynamic parsing
expr='1 + 2'
a[expr]=55

b=(42)
expr='b[0]'
a[3 + $expr - 4]=66

typeset -p a
