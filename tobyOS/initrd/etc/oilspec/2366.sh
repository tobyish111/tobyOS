# http://landley.net/notes.html#12-06-2020

rm -f walrus
$SH -c 'X=${x?bc} > walrus'
if test -f walrus; then echo 'exists1'; fi

rm -f walrus
$SH -c '>walrus echo ${a?bc}'
test -f walrus
if test -f walrus; then echo 'exists2'; fi
