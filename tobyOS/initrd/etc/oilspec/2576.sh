f() {
  argv.py "${!1}"
}
f 'nonexistent[0]'
array=(x y z)
f 'array[0]'
f 'array[1+1]'
f 'array[@]'
f 'array[*]'
# Also associative arrays.
