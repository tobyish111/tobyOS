flags='-e'
case $SH in dash) flags='' ;; esac

echo $flags 'abcd\U00000065f'
