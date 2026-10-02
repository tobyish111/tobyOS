mkdir -p d1; cd d1 d2; echo st=$?; pwd | sed 's,.*/,,'
x=1; export x; export -n x; declare -p x; export -n zz; echo st=$?; export -n y=2; declare -p y
eval() { echo "eval func $*"; }; eval 'echo hi'; unset -f eval
let x=( 1 ); let y=( x + 2 ); echo $x $y
for - in a; do echo no; done; echo st=$?
for x in a b; do echo $x; continue 1 2; done; echo after-loop
declare -A A; A['x']=42; (( q = A['x'] )); echo q=$q
(( A['y'] = 7 )); echo ${A[y]}
a=(4 5 6); (( b = a )); echo b=$b
declare undef; unset -v 'undef[1]'; echo st=$?; unset -v 'undef["key"]'; echo st=$?
declare -A dict=(); key='1],a[1'; dict["$key"]=foo; unset -v 'dict["$key"]'; echo n=${#dict[@]}
s='foo()'; case $s in *\(\)) echo match;; esac; [[ $s == *\(\) ]] && echo m2
