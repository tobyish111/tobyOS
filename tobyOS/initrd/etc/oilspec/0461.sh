case $SH in dash) exit 99 ;; esac # dash/mksh does not support associative arrays

a=(1 2 3)
readonly -a a
eval 'a+=(4)'
argv.py "${a[@]}"
eval 'declare -n r=a; r+=(4)'
argv.py "${a[@]}"
