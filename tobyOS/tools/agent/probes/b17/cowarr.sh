# cowarr.sh: a subshell's arrays are its own (finding 41's copy-on-write).
# Every shape of change inside ( ) and $( ), nested, and what the parent
# sees after.
exec 2>&1
p() { declare -p "$@" 2>&1; }
a=(1 2 3); declare -A m=([x]=1 [y]=2)
( a[1]=X; unset 'a[2]'; m[z]=3; unset 'm[x]' ); p a m
x=$( a+=(4); m[w]=9; echo in ); p a m
( unset a; a=(new); declare -A m=([q]=q) ); p a m
( b=(made inside) ); p b
( ( a[0]=deep ); echo "mid ${a[0]}"; a[0]=mid ); p a
( a[0]=one; ( a[0]=two ); echo "after inner: ${a[0]}" ); p a
y=$( z=$( a[2]=inner; echo "${a[*]}" ); echo "z=$z a=${a[*]}" ); echo "$y"; p a
f() { local -a a=(loc); a[1]=more; echo "f: ${a[*]}"; }
( f; echo "sub after f: ${a[*]}" ); p a
g() { a[5]=g; }
( g ); p a
x=$( mapfile -t a <<< $'p\nq'; echo "${#a[@]}" ); echo "mapfile in: $x"; p a
echo hi | { read -a a; echo "read: ${a[*]}"; }; p a
a[1]=stage | true; p a
c=(c1 c2); ( c=(); ); p c
d=([5]=five); ( d[7]=seven; unset 'd[5]' ); p d
( unset 'a[@]' ); p a
( declare -a e=(e1); e[3]=e3 ); p e 2>&1 | sed 's/^.*: line [0-9]*: //'
( pushd / >/dev/null; echo "ds ${#DIRSTACK[@]}" ); echo "ds after ${#DIRSTACK[@]}"
( alias zz=yy; echo "${BASH_ALIASES[zz]}" ); echo "alias after [${BASH_ALIASES[zz]-unset}]"
cat <<EOF >/dev/null
$( a[0]=heredoc )
EOF
p a
for i in 1 2 3; do x=$( a[0]=loop$i ); done; p a
h=(h); ( h+=(x); ( h+=(y); ( h+=(z); echo "${h[*]}" ); echo "${h[*]}" ); echo "${h[*]}" ); p h
echo end
