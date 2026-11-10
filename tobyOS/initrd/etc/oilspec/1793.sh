check() {
  argv.py "${FUNCNAME}"
  argv.py "${#FUNCNAME}"
  argv.py "${FUNCNAME::1}"
  argv.py "${FUNCNAME:1}"
}
check
