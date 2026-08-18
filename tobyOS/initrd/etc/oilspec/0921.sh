echo '
HISTFILE=(a b c)
history -a
echo status=$?
' | $SH -i

case $SH in bash) echo '^D' ;; esac

# note that bash actually writes the file 'a', since that's ${HISTFILE[0]} 
