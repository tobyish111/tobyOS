flags='-e'
case $SH in dash) flags='' ;; esac

echo $flags 'abcd\044e'
