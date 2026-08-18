unlocal() { unset -v "$1"; }

f2() {
  local v=local1
  v=tempenv2 eval '
    local v=local2
    (unset v  ; echo "[$1,local1,tempenv2,(eval),local2,(unset)] v: ${v-(unset)}")
    (unlocal v; echo "[$1,local1,tempenv2,(eval),local2,(unlocal)] v: ${v-(unset)}")
  '
}
v=global
v=tempenv1 f2 global,tempenv1


# Note that bash-4.3 to bash 5.0 behave differently
# [global,tempenv1,local1,tempenv2,(eval),local2,(unset)] v: local1
# [global,tempenv1,local1,tempenv2,(eval),local2,(unlocal)] v: local1

# always-value-unset
[global,tempenv1,local1,tempenv2,(eval),local2,(unset)] v: (unset)
[global,tempenv1,local1,tempenv2,(eval),local2,(unlocal)] v: (unset)
