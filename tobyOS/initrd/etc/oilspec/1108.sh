case $SH in dash) exit ;; esac

read -t 0.5 < /dev/null
echo $?
