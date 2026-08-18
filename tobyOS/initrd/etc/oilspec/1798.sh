# note: it's CALLS, not DEFINITIONS.
g() {
  argv.py G "${BASH_LINENO[@]}"
}
f() {
  argv.py 'begin F' "${BASH_LINENO[@]}"
  g  # line 6
  argv.py 'end F' "${BASH_LINENO[@]}"
}
argv.py ${BASH_LINENO[@]}
f  # line 9
