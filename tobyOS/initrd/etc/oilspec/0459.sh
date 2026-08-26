case $SH in dash|mksh) exit 99 ;; esac # dash/mksh does not support associative arrays

declare -a arr1
readonly -a arr2
declare -A dict1
readonly -A dict2

declare -p arr1 arr2 dict1 dict2
