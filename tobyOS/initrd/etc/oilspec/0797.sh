case $SH in bash) exit ;; esac

COMP_ARGV=({0..9})
unset -v 'COMP_ARGV['{1,3,4,6,7,8}']'
compadjust words
argv.py "${words[@]}"
