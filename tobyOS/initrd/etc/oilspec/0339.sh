declare -a myarray
argv.py "${myarray[@]}"
myarray+=('x')
argv.py "${myarray[@]}"

f() {
  local -a myarray
  argv.py "${myarray[@]}"
  myarray+=('x')
  argv.py "${myarray[@]}"
}
f
