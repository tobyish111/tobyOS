test_var1=global
f1() {
  local test_var1=local
  {
    declare -pg
  } | grep -E '^\[|^\b[^"]*test_var.\b'
}
f1
