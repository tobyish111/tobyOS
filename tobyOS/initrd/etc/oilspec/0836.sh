flags='-en'
case $SH in dash) flags='-n' ;; esac

echo $flags '\0777' | od -A n -t x1 | sed 's/ \+/ /g'
