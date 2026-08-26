printf "echo %s\n" {1..3} > tmp

echo '
HISTFILE=tmp
history -c
history -r

fc -l 3 2
' | $SH --norc -i

# match osh's behaviour of echoing ^D for EOF
case $SH in bash) echo '^D' ;; esac
