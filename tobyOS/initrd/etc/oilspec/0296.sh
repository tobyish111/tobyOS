# mksh support local variables, but not local arrays, oddly.
f() {
  local a=(1 '2 3')
  argv.py "${a[0]}"
}
f
