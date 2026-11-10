f() {
  echo one
  eval 'return'
  echo two
}
f
