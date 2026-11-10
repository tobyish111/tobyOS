# A local that SHADOWS an array of the caller's: the caller's array must
# come back exactly -- associative, empty, sparse.
declare -A m=([a]=1 [b]=2)
f() { local m=x; echo "in [$m]"; }
f; declare -p m
g() { local -A m; m[z]=9; declare -p m; }
g; declare -p m
e=()
h() { local e=1; }
h; declare -p e
s=([3]=x [9]=y)
k() { local -a s=(1); }
k; declare -p s
echo end
