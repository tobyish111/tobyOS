case $SH in zsh|ash) exit ;; esac

i=(0 1 2)

a[i[0]]=0
a[ i[1] ]=1
a[ i[2] ]=2
a[ i[1]+i[2] ]=3

argv.py "${a[@]}"
