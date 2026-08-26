case $SH in dash|mksh) exit ;; esac

shopt -o | egrep -o 'errexit|noglob|nounset'
echo --
