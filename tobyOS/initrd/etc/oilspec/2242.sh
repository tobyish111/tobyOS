case $SH in dash|mksh) exit ;; esac

shopt -p > /dev/null
echo status=$?
