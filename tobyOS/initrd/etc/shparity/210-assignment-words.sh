# Assignment WORDS: NAME+= and NAME[SUB]= are assignments whose values are
# not split; a prefix's later words and a declaration utility written as
# such (not `$m` that expands to one) take assignments; declare -a/-A
# convert a scalar (or refuse an array of the other kind); a literal's
# [k]=v keys may index the array, brace-expand for an indexed array, and
# take no tilde in an associative one; `let x=( 1 )` is one word.
exec 2>&1
x='a b'; s=1; s+=$x; echo "[$s]"
y='c d'; t=$y; echo "[$t]"
a=(1); a+=$x; declare -p a
a[0]+=$x; declare -p a
HOME=/home/user
a=([k2]=-{a,b}-); declare -p a
b=([1]=~ [2]=~:~ ~ x=~); declare -p b
declare -A c=([h]=~ [g]=x{1,2}y); declare -p c
x=v; declare -A x; declare -p x
y=v; declare -A y+=([a]=1); declare -p y
z=v; declare -A z=([a]=1); declare -p z
w=v; declare -a w; declare -p w
q=(1 2); declare -A q 2>&1; echo st=$?; declare -p q
myvar=typeset; v='a b'; $myvar v2=$v; declare -p v2
p=1 export e1=$v; declare -p e1
command export e2=$v; declare -p e2
builtin export e3=$v; declare -p e3
