f() {
  echo $g_var
}
g() {
  local g_var=g_var
  f
}
g
