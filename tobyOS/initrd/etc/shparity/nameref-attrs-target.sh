# declare's attributes and export given to an existing reference go to the
# variable at the end of its chain -- -i -l -u -a -A -x, +x, export -n --
# unless the same declare makes it a reference; in a function only the
# function's own reference passes them on (finding 322). tsh put them on
# the reference.
{
declare -n r=t; declare -i r; r=4+4; declare -p r t
declare -n s=u; declare -u s=abc; declare -p s u; declare -l s; s=XY; declare -p u
declare -n p=v; declare -A p; p[k]=w; declare -p p v; declare -n o=m; declare -a o; o[1]=w; declare -p o m
x=1; declare -n rx=x; export rx; declare -p rx x; env | grep -c '^x='; declare +x rx; declare -p x
declare -n ry=y; export ry=7; declare -p ry y; export -n ry; declare -p ry y
a=(1 2); declare -n e='a[1]'; declare -i e; declare -p a e
T=1; declare -n R=T; f() { declare -i R; R=2+2; declare -p R T; }; f; declare -p R T
g() { declare -x R=9; declare -p R T; }; g; declare -p R T
h() { declare -gi R; R=3+3; }; h; declare -p R T
k() { local -n L=T; declare -i L; L=5+5; declare -p L T; }; k; declare -p T
} 2>&1
