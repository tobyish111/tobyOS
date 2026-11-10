case $SH in bash|mksh) exit ;; esac

a=({0..5})
unset -v 'a[1]' 'a[2]' 'a[4]'

shopt -s parse_at
argv.py @[a]
argv.py @a
