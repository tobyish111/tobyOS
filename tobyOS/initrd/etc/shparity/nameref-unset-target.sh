# `unset ref` unsets what ref names and keeps the reference -- as `unset -v
# TARGET` would: a readonly target is refused, status 1, a caller's local
# goes and the outer value shows, a name that is only a function stays;
# `unset -n NAME` of a non-reference does nothing (finding 319). tsh
# removed the reference too, and emptied a readonly array.
{
t=1; declare -n r=t; unset r; echo "rc=$? [${t-unset}] [${r-unset}] [${!r}]"; declare -p r; unset -n r; declare -p r
( readonly t=2; declare -n r=t; unset r; echo "rc=$? [$t] [$r]"; declare -p r )
( readonly -a ra=(1 2); declare -n r=ra; unset r; echo "rc=$?"; declare -p ra )
a=(1 2); declare -n q=a; unset q; declare -p q; declare -p a
x=42; declare -n f='x[0]'; unset f; echo "rc=$? [${x-unset}]"; x=43; declare -n g='x[1]'; unset g; echo "rc=$? [$x]"
fn() { echo fn; }; declare -n h=fn; unset h; echo "rc=$?"; fn; declare -p h
f() { local t=loc; g; echo "f [${t-unset}]"; }; g() { declare -n r=t; unset r; echo "g [${t-unset}]"; }; t=glob; f; echo "top [$t]"
u=2; unset -n u; echo "rc=$? [${u-unset}]"
t=1; declare -n r=t; declare -n s=r; unset s; echo "[${t-unset}]"; declare -p r s
} 2>&1
