case $SH in mksh|dash) exit ;; esac

builtin kill -L 10 BAD 12
echo status=$?
echo

builtin kill -L USR1 9999 USR2
echo status=$?
echo
