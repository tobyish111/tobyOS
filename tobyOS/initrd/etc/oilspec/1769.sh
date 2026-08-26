# This is analogous to the 'while' case in spec/loop
f() {
  if break; then
    echo hi
  fi
}
f
