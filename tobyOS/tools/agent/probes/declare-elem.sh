# declare/local/typeset/readonly/export with an array ELEMENT as the name.
exec 2>&1
declare a[1]=x; declare -p a
declare -a b[2]=y; declare -p b
declare -A m[k]=v; declare -p m
declare c[1/0]=z; echo "n1 $?"
echo "s1 $?"
f() { local d[1]=q; declare -p d; }; f; declare -p d
declare e[1]; declare -p e
declare -i g[0]=1+2; declare -p g
h=(1 2); declare h[1]+=x; declare -p h
typeset t[3]=w; declare -p t
readonly r[1]=x; declare -p r; echo "s2 $?"
export x[1]=y; echo "s3 $?"; declare -p x 2>&1
declare 'q[1]=a b' ; declare -p q
v=V; declare w[\$v]=1; declare -p w
declare -a k[0]=(1 2); declare -p k; echo "s4 $?"
declare z[]=1; echo "s5 $?"
declare -p nosuch[1]; echo "s6 $?"
echo end
