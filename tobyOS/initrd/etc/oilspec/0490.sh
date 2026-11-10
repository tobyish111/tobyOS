unlocal() { unset -v "$1"; }

f1() {
  local v=local
  unset v
  echo "[$1,local,(unset)] v: ${v-(unset)}"
}
v=global
v=tempenv f1 'global,tempenv'

f1() {
  local v=local
  unlocal v
  echo "[$1,local,(unlocal)] v: ${v-(unset)}"
}
v=global
v=tempenv f1 'global,tempenv'


# Note on bug in bash 4.3 to bash 5.0
# [global,tempenv,local,(unset)] v: global
# [global,tempenv,local,(unlocal)] v: global


# always-value-unset
#   local-unset   = value-unset
#   dynamic-unset = value-unset
[global,tempenv,local,(unset)] v: (unset)
[global,tempenv,local,(unlocal)] v: (unset)
