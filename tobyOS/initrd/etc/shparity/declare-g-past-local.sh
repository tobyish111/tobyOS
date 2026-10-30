# declare -g in a function of a name a local hides is the GLOBAL variable's:
# its value and attributes, a global reference followed, a readonly mark
# kept after the local goes; an element operand stays the local's
# (finding 360). tsh wrote the local.
exec 2>&1
g=glob; f() { local g=loc; declare -g g=new; echo "in [$g]"; }; f; echo "out [$g]"
n=1; f() { local n=loc; declare -gi n=3+4; echo "in [$n]"; }; f; declare -p n
arr=(x); f() { local arr=loc; declare -ga arr=(1 2); echo "in [${arr[*]}]"; }; f; declare -p arr
x=1; f() { local x=loc; declare -gx x=2; }; f; declare -p x
s=ab; f() { local s=loc; declare -g s+=cd; }; f; echo "[$s]"
t=top; h() { declare -g t=new; echo "h [$t]"; }; f() { local t=mid; h; echo "f [$t]"; }; f; echo "top [$t]"
e=(a b); f() { local e=loc; declare -g e[1]=B; echo "in [$e]"; }; f; declare -p e
r=1; f() { local r=loc; declare -gr r=2; }; f; echo "[$r]"; r=4; echo "not here"
echo "rc=$? [$r]"
