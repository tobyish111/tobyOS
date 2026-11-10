f() {
  g_var=f_mutation
}
g() {
  local g_var=g_var
  echo "g_var=$g_var"
  f
  echo "g_var=$g_var"
}
g
echo g_var=$g_var
