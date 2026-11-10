test -v nonexistent
echo global=$?

g=1
test -v g
echo global=$?

f() {
  local f_var=0
  g
}

g() {
  test -v f_var
  echo dynamic=$?
  test -v g
  echo dynamic=$?
  test -v nonexistent
  echo dynamic=$?
}
f
