case $SH in dash) exit ;; esac

set -u

typeset -a empty
empty=()

echo empty /"${empty[@]}"/
echo undefined /"${undefined[@]}"/



# empty array is unset in mksh
