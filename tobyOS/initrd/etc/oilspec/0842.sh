# \0 is special, but \1 isn't in bash
# \1 is special in dash!  geez
flags='-en'
case $SH in dash) flags='-n' ;; esac

echo $flags '\0' '\1' '\8' | od -A n -c | sed 's/ \+/ /g'
