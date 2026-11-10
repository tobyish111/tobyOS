case $SH in dash) exit ;; esac

s='42'
echo 42 $(( s[0] )) $(( s[1] ))
echo status=$?
