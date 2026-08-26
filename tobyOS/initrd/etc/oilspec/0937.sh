case $SH in mksh|dash) exit ;; esac

builtin kill -l 10 11 12
echo status=$?
echo

builtin kill -l SIGUSR1 SIGSEGV USR2
echo status=$?
echo

# mixed kind
builtin kill -l 10 SIGSEGV 12
echo status=$?
echo
