# unset across function scopes, as bash's makunbound: a local of the CURRENT
# function stays local and unset; a CALLER's local, or a prefix binding
# (`v=x f`), is removed and what it hid comes back; `local v` with no
# value takes a prefix binding's value. tsh unset whatever the flat table
# held, so the caller's local -- and the prefix value -- simply vanished.
exec 2>&1
unlocal() { unset "$@"; }
l2() { local h=yy; echo "l2=$h"; unlocal h; echo "l2=$h"; }
l1() { local h=xx; l2; echo "l1=$h"; unlocal h; echo "l1=$h"; l2; }
h=global; l1; echo "top=$h"

f1() { local v=local; unset v; echo "own: ${v-(unset)}"; v=again; echo "own2: $v"; }
v=global; f1; echo "after own: $v"
f2() { local v=local; unlocal v; echo "caller: ${v-(unset)}"; v=new; }
v=global; f2; echo "after caller: $v"

f3() { unset v; echo "tempenv: ${v-(unset)}"; v=set-in-f3; }
v=global; v=tempenv f3; echo "after tempenv: $v"
f4() { unlocal v; echo "tempenv unlocal: ${v-(unset)}"; }
v=global; v=tempenv f4; echo "after: $v"

f5() {
  echo "[$1] v: ${v-(unset)}"
  local v
  echo "[$1,local] v: ${v-(unset)}"
  ( unset v; echo "[$1,local+unset] v: ${v-(unset)}" )
  ( unlocal v; echo "[$1,local+unlocal] v: ${v-(unset)}" )
  unlocal v; echo "[$1,unlocal-here] v: ${v-(unset)}"
}
v=global
f5 global
v=tempenv f5 global,tempenv
v=tempenv eval 'f5 "global,tempenv,(eval)"'
echo "end v=$v"

g1() { local v; echo "[$1,(local)] v: ${v-(unset)}"; }
g2() { g1 "$1,(func)"; }
g3() { local v=local; g1 "$1,local,(func)"; }
v=global
g1 global; v=tempenv g1 global,tempenv; (export v=global; g1 xglobal)
g2 global; v=tempenv g2 global,tempenv
g3 global

k1() { v=4 local v; echo "k1: ${v-(unset)}"; }
v=1; k1; echo "after k1: $v"
v=global; v=tmp unset v; echo "builtin tempenv: ${v-(unset)}"
m1() { v=tmp unset v; echo "m1: ${v-(unset)}"; }
v=global; m1; echo "after m1: ${v-(unset)}"
m2() { local v=L; v=tmp unset v; echo "m2: ${v-(unset)}"; }
v=global; m2
d1() { declare v; echo "d1: ${v-(unset)}"; }
v=global; v=tempenv d1
a1() { local -a arr=(x y); a2; echo "a1: ${arr[*]-(unset)}"; }
a2() { unset arr; echo "a2: ${arr[*]-(unset)}"; }
arr=(g1 g2); a1; echo "top arr: ${arr[*]}"
