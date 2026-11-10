case $SH in mksh) exit ;; esac

a=(v{0..9})
unset -v 'a[2]' 'a[3]' 'a[4]' 'a[7]'

argv.py "${a[@]#v}"
argv.py "abc${a[@]#v}xyz"
argv.py "${a[@]%[0-5]}"
argv.py "abc${a[@]%[0-5]}xyz"
argv.py "${a[@]#v?}"
