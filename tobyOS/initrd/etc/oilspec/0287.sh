case $SH in bash|mksh) exit ;; esac

COMP_ARGV=(echo 'Hello,' 'Bash' 'world!')
compadjust cur prev words cword
argv.py "$cur" "$prev" "$cword"
argv.py "${words[@]}"
