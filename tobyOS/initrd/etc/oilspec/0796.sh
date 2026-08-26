case $SH in bash) exit ;; esac

COMP_ARGV=()
compadjust words
argv.py "${words[@]}"
