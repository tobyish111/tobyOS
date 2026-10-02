# A VALUE that begins with `(` is a string unless the `(` was written there:
# read, for, printf -v, getopts, `$_`, quoted and expanded assignments all
# made an ARRAY of it in tsh -- `x='(a b)'` was x=([0]=a [1]=b). With -a/-A
# (or an existing array) `declare` takes a whole (...) value as a list.
exec 2>&1
echo "(( x"; echo "1 $_"
for v in "(a b)"; do declare -p v; done
read r <<< "(a b)"; declare -p r
printf -v p '(%s)' 'a b'; declare -p p
x='(a b)'; y=$x; declare -p y
getopts 'a:' o -a '(q)'; declare -p OPTARG
f() { echo "(last word)"; }; f; echo "2 $_"
mapfile -t m <<< "(m n)"; declare -p m
: "(u v)"; echo "3 $_"
x='(a b)'; declare -p x
x2="(a b)"; declare -p x2
x3=\(a\ b\); declare -p x3
read z <<< "(c d)" ; declare -p z
z2=$(echo "(e f)"); declare -p z2
w="(g h)"; w2=$w; declare -p w2
w3="$w"; declare -p w3
export e1=$w; declare -p e1
declare d1=$w; declare -p d1
local_t() { local l1=$w; declare -p l1; }; local_t
q=$w env | grep '^q='
x='(1 2)'
declare -a a=$x; declare -p a
declare b=$x; declare -p b
declare -a c="$x"; declare -p c
declare -A e='([k]=v)'; declare -p e
declare f='(3 4)'; declare -p f
declare -a g='(5 6)'; declare -p g
f2() { local -a d=$x; declare -p d; local h=$x; declare -p h; }; f2
export k='(7 8)'; declare -p k
readonly r='(9 0)'; declare -p r
n=(1); n=$x; declare -p n
typeset -a t=\(a\ b\); declare -p t
declare -a u=('(p q)'); declare -p u
