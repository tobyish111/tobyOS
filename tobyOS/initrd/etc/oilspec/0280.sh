case $SH in mksh) exit ;; esac

a=(v{0..9})
unset -v 'a[2]' 'a[3]' 'a[4]' 'a[7]'

argv.py "${a[@]@P}"
argv.py "${a[*]@P}"
argv.py "${a[@]@Q}"
argv.py "${a[*]@Q}"
argv.py "${a[@]@a}"
argv.py "${a[*]@a}"
