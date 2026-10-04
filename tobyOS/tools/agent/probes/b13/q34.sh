exec 2>&1 </dev/null
set -x
a=(  1   2  )
a=([0]=x [3]="y z"   'q r')
a=(
 1
 2 # c
)
b=$'x\ty'
c='it'"'"'s'
d="'"
e='a=b'
f=~
for i in 'a b' "c d" $(echo e); do :; done
set -- p q
for i; do :; done
for i in; do :; done
for (( i = 0 ; i < 1 ; i++ )); do :; done
for ((;;)); do break; done
for (( ; i < 2 ; )); do i=2; done
x=5; (( $x + 1 )); (( x ))
[[ a == "*" ]]
[[ a == * ]]
[[ a == 'b c' ]]
[[ "" == "" ]]
[[ -z "" ]]
[[ a < b ]]
[[ ( a == a ) ]]
[[ ! ( a == b ) ]]
[[ a =~ "x y" ]]
[[ a != b || c == d ]]
case "a b" in *) : ;; esac
case 'x' in *) : ;; esac
v=$(echo $(echo $(echo deep3)))
eval 'v=$(echo inev)'
h() { echo "$(echo infn)"; }; h
export ex1=1 ex2
readonly ro1=1
declare -x dx=1
local_f() { local -a la=(1 2); declare -A m=([k]=v); typeset ty=(z); export arrx=(p); }
local_f
declare -i di=1+1
set +x
echo end
