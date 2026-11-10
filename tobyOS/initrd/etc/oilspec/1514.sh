set -o errexit
f() {
  echo good
  local x=$(echo one; false; echo two)
  echo status=$?
  echo $x
}
f
# for dash and mksh, the INNER shell aborts, but the outer one keeps going!
