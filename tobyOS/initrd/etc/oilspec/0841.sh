flags='-en'
case $SH in dash) flags='-n' ;; esac

echo $flags '\u6' | od -A n -c | sed 's/ \+/ /g'
