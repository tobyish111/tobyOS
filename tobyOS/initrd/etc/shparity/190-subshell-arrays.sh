# A SUBSHELL'S ARRAYS ARE ITS OWN: ( ) and $( ) run in-process in /bin/tsh,
# in a frame that restored variables, functions, traps and descriptors but
# not arrays -- an element set, an array made or one unset inside stayed
# that way outside. And `x=1 f` binds x for the call only: tsh put the
# whole environment back afterwards, so everything f itself set was lost.
exec 2>&1
a=(1); ( a[1]=2 ); declare -p a
( b=(x y) ); declare -p b
c=(1 2); ( unset c ); declare -p c
declare -A m; m[k]=v; ( m[j]=w ); declare -p m
x=$( d=(p q); echo ${#d[@]} ); declare -p d
echo z | { e=(1 2 3); }; declare -p e
f() { y=2; a2=(1); }; x=1 f; echo "[$y] [${a2[*]}] [${x-unset}]"
( g=(7) ; ( g[1]=8; declare -p g ); declare -p g ); declare -p g 2>&1
h=(1 2 3); for i in 1 2; do v=$( h[i]=x; echo "${h[*]}" ); echo "$v / ${h[*]}"; done
mm=(1); echo q | { mm[1]=2; declare -p mm; }; declare -p mm
f() { y=2; x=9; export z=3; a=(q); }; x=0; x=1 f; echo "[$y] [$x] [$z] [${a[*]}]"
g() { echo "in [$x]"; }; x=1 g; echo "out [$x]"
h() { local x=5; w=6; }; x=1 h; echo "[$x] [$w]"
k() { unset y; }; y=1 k; echo "[${y-unset}]"
export E=1; m() { printenv E; }; E=2 m; printenv E
n() { x=7; }; unset x; x=1 n; echo "[${x-unset}]"
