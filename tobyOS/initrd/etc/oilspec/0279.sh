case $SH in mksh) exit ;; esac

a=(v{0..9})
unset -v 'a[2]' 'a[3]' 'a[4]' 'a[7]'

argv.py "${a[@]/?}"
argv.py "${a[@]//?}"
argv.py "${a[@]/#?}"
argv.py "${a[@]/%?}"

argv.py "${a[@]/v/x}"
argv.py "${a[@]//v/x}"
argv.py "${a[@]/[0-5]/D}"
argv.py "${a[@]//[!0-5]/_}"
