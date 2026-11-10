case $SH in dash) exit ;; esac

s='abc'
echo abc $(( s[0] )) $(( s[1] ))
echo status=$?
echo

(( s[0] ))
echo status=$?
echo
