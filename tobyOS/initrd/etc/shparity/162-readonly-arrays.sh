# Readonly ARRAYS (S4): every write path refuses, with bash's statuses, and
# `readonly -a/-A` are accepted. tsh rejected `readonly -a` as a bad name and
# then let every one of these writes through with status 0. In a script an
# assignment error ends the rest of ITS LINE (bash), so each write stands on
# its own line and the status is printed on the next.
readonly -a r=(1 2)
r=(3)
echo "assign $? [${r[*]}]"
r[0]=9
echo "element $? [${r[*]}]"
r[1]+=x
echo "element-append $? [${r[*]}]"
r+=(4)
echo "append $? [${r[*]}]"
unset 'r[0]'
echo "unset-element $? [${r[*]}]"
unset r
echo "unset $? [${r[*]}]"
declare -a r=(7)
echo "declare $? [${r[*]}]"
read -a r <<< "a b"
echo "read-a $? [${r[*]}]"
mapfile -t r <<< "m"
echo "mapfile $? [${r[*]}]"
printf -v 'r[1]' %s zz
echo "printf-v $? [${r[*]}]"
declare -p r
declare -A m=([k]=v); readonly m
m[k]=w
echo "assoc-element $? [${m[k]}]"
m+=([j]=u)
echo "assoc-append $? [${!m[*]}]"
readonly -A n=([x]=1)
declare -p n
readonly -a e
declare -p e
r=(5); echo "same line: not reached"
echo "next line runs: $?"
declare -a r=(8); echo "declare, same line: not reached"
echo "after declare: $?"
x=1; readonly x
x=2; echo "scalar same line: not reached"
echo "after scalar: $? [$x]"
x=3 true
echo "prefix: $? [$x]"
readonly -f g 2>/dev/null; echo "readonly -f unknown: $?"
g() { echo g; }; readonly -f g
g() { echo h; }
g; echo "redefine: $?"
unset -f g
g; echo "unset -f: $?"
f() { local -a r=(9); echo "local in f: $? [${r[*]}]"; }
f
echo "after f: $? [${r[*]}]"
readonly -p | grep -c 'r='
echo done
