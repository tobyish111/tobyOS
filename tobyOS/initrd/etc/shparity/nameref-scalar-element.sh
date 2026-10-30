# Element 0 of a scalar is the scalar -- through a reference to b[0], and in
# ${!x[0]}; an unset element of a variable that is set expands to nothing,
# and only a name that is no variable at all is "NAME[SUB]: invalid
# indirect expansion" (finding 324). tsh read "" and called both invalid.
{
b=4; declare -n f='b[0]'; echo "[$f] [${f}]"; f=5; echo "[$b]"
t=v; v=VV; declare -n r=t; echo "[${!r[0]}] [${!r[@]}]"; a=(x y); declare -n q=a; echo "[${!q[0]}] [${!q[@]}]"
x=y; y=Y; echo "[${!x[0]}] [${!x[1]}] [${!x[1]-dflt}] [${!x[1]+alt}]"
c=(p); echo "[${!c[3]}] [${!c[3]-d}]"; declare -A A=([k]=c); echo "[${!A[k]}] [${!A[z]-d}]"
} 2>&1
declare -n z=nothere; { echo "[${!z[0]}]"; } 2>&1; echo "after rc=$?"
{ echo "[${!nosuch[0]}]"; } 2>&1; echo "after rc=$?"
