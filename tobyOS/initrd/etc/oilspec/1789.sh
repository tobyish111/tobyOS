g() {
  argv.py "${FUNCNAME[@]}"
}
f() {
  argv.py "${FUNCNAME[@]}"
  g
  argv.py "${FUNCNAME[@]}"
}
f
