# http://landley.net/notes.html#15-03-2020
$SH -c 'echo $((echo hello))'
if test $? -ne 0; then echo fail; fi
