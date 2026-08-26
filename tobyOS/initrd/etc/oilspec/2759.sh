set -x
echo one
f() { 
  local PS4='- '
  echo func;
}
f
echo two
