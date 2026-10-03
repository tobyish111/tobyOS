B=/home/toby/bash-5.2.15/bash
while IFS= read -r line; do
  out=$($B -c "$line" 2>&1); echo "[$line] => rc=$? out=$(echo $out)"
done <<'EOF'
[[ -n == ]] && echo y
[[ -n == && -n x ]]
[[ "-f" x ]]
[[ \-f x ]]
[[ -f"" x ]]
var=-f; [[ $var == -f ]] && echo y
var=-f; [[ $var ]] && echo y
u=-z; [[ $u x ]]
op="=="; [[ a $op a ]]
[[ a "==" a ]]
[[ a \== a ]]
[[ -n "" ]] && echo y
[[ ! -n x ]]; echo $?
x='!'; [[ $x -n y ]]; echo $?
[[ a '<' b ]]; echo $?
[[ ( -n x ) ]] && echo y
p='('; [[ $p -n x ) ]]; echo $?
EOF
