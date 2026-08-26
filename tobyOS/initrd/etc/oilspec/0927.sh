shopt -s histappend
echo status=$?
shopt -p histappend
shopt -u histappend
echo status=$?
shopt -p histappend

# match osh's behaviour of echoing ^D for EOF
case $SH in bash) echo '^D' ;; esac
