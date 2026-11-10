unlocal() { unset -v "$1"; }

f1() {
  local v=local
  unset v
  echo "[$1,local,(unset)] v: ${v-(unset)}"
}
v=global
f1 global

f1() {
  local v=local
  unlocal v
  echo "[$1,local,(unlocal)] v: ${v-(unset)}"
}
v=global
f1 'global'



# always-value-unset
#   local-unset   = value-unset
#   dynamic-unset = value-unset
[global,local,(unset)] v: (unset)
[global,local,(unlocal)] v: (unset)
