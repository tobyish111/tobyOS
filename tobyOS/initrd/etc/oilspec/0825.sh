flags='-e'
case $SH in dash) flags='' ;; esac

echo $flags 'foo\
bar'
