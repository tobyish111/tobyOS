case $SH in mksh) exit ;; esac

a=(v{0..9})
unset -v 'a[3]' 'a[4]' 'a[7]' 'a[9]'

argv.py "${!a[@]}"
