case $SH in dash) exit ;; esac

echo ARITH $(( undef[0] ))
echo status=$?
echo

(( undef[0] ))
echo status=$?
echo

echo UNDEF ${undef[0]}
echo status=$?
