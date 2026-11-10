f3() {
  local -n ref=$1
  ref=x
}

f2() {
  f3 "$@"
}

f1() {
  local F1=F1
  echo F1=$F1
  f2 F1
  echo F1=$F1
}
f1
