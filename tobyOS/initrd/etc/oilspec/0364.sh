f() {
  local a[3]=4 a[5]=6
  echo status=$?
  argv.py "${!a[@]}" "${a[@]}"
}
f
