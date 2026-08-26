__GLOBAL=g
f() {
  local __mylocal=L
  local __OTHERLOCAL=L
  __GLOBAL=mutated
  set | grep '^__'
}
g() {
  local __var_in_parent_scope=D
  f
}
g
