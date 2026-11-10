flags='-e'
case $SH in dash) flags='' ;; esac

echo $flags 'abcd\u0065f'
