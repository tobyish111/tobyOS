flags='-en'
case $SH in dash) flags='-n' ;; esac

echo $flags 'abcd\04' | od -A n -c | sed 's/ \+/ /g'
