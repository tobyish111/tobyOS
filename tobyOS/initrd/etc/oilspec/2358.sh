HOME=/home/bar
f() {
  local x=foo:~
  echo $x
}
f
