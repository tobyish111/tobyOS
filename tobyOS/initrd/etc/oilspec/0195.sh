array=('1 2' 3)
declare -A d
d['key']="${array[@]}"
argv.py "${d['key']}"
