# A quote or backslash right after a name continues the word, and a quoted
# word is no alias: with ec=echo, `ec"x"` is the command ecx and `ec''` the
# command ec -- tsh replaced the ec (finding 472).
exec 2>&1
shopt -s expand_aliases
alias ec=echo
ec"x" 2>&1 | sed 's/^.*line [0-9]*: //'
ec\x 2>&1 | sed 's/^.*line [0-9]*: //'
ec'' y 2>&1 | sed 's/^.*line [0-9]*: //'
ec ok
