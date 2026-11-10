# Parse error -- parsed BEFORE evaluation of vars
op='=='
[[ a $op a ]] && echo true
[[ a $op b ]] || echo false
