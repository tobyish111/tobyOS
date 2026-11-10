case $SH in zsh|mksh|ash) exit ;; esac

# OSH doesn't allow this
declare a[a[0]=1]=X
declare -p a

# neither bash nor OSH allow this
declare a[ a[2]=3 ]=Y
declare -p a
