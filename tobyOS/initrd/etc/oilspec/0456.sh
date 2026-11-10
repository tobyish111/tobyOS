case $SH in dash) exit 99 ;; esac # dash does not support arrays

declare -a arr
arr=(foo bar baz)
declare -a arr
echo arr:${#arr[@]}
