case $SH in dash) exit ;; esac

set -o nounset
echo UNSET $(( undef[0] ))
echo status=$?
