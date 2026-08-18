shopt -s expand_aliases
alias e_='echo 1
echo 2
echo 3
'
var='echo foo'
e_ ${var}
