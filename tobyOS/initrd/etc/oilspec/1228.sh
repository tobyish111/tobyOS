case $SH in dash) exit ;; esac
trap "echo INT" INT
trap "echo EXIT" EXIT
trap -p
