case $SH in zsh|ash) exit ;; esac

a[1 * 1]=x
a[ 1 + 2 ]=z
echo status=$?

argv.py "${a[@]}"
