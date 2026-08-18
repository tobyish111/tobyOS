case $SH in zsh|mksh|ash) exit ;; esac

# the nested [] means we can't use regular language lookahead?

echo assign=$(( z[0] = 42 ))

a[a[0]=1]=X
declare -p a

a[ a[2]=3 ]=Y
declare -p a

echo ---

a[ a[0]+=1 ]+=X
declare -p a
