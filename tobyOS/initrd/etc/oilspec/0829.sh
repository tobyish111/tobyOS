flags='-e'
case $SH in dash) flags='' ;; esac

echo $flags xy  'ab\cde'  'zzz'
