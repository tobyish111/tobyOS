declare -A A
A['x']='foo'
A['x']+='bar'
A['x']+='bar'
argv.py "${A["x"]}"
