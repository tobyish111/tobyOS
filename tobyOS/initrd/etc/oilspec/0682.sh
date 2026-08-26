# OK -- parsed AFTER evaluation of vars
op='='
[ a $op a ] && echo true
[ a $op b ] || echo false
