echo '
if test -n $HISTFILE; then echo exists; fi
' | $SH -i

# match osh's behaviour of echoing ^D for EOF
case $SH in bash) echo '^D' ;; esac
