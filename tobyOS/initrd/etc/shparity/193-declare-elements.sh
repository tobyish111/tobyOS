# declare/local/typeset/readonly/export with an array ELEMENT as the name,
# as bash's declare_internal has it: `declare a[1]=x` assigns the element,
# `declare -a k[0]=(1 2)` cannot assign a list to a member. tsh called every
# such word "not a valid identifier".
exec 2>&1
declare a[1]=x; declare -p a
declare -a b[2]=y; declare -p b
declare -A m[k]=v; declare -p m
f() { local d[1]=q; declare -p d; }; f; declare -p d
declare e[1]; declare -p e
declare -i g[0]=1+2; declare -p g
h=(1 2); declare h[1]+=x; declare -p h
typeset t[3]=w; declare -p t
declare 'q[1]=a b' ; declare -p q
declare -a k[0]=(1 2); declare -p k; echo "s4 $?"
echo end
