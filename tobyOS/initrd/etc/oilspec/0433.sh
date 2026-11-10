set -o nounset
f() {
  local -a x
  echo $x
}
f
