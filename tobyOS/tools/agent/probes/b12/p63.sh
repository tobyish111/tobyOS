B=/home/toby/bash-5.2.15/bash
while IFS= read -r line; do
  out=$($B -c "$line" 2>&1); rc=$?; echo "[$line] => rc=$rc out=$(echo $out)"
done <<'EOF'
[[ $(echo a)]] && echo y
[[ (a)]] && echo y
[[ !(a == b)]] && echo y
[[ ! (a == b)]] && echo y
[[ a == @(a|b) ]] && echo y
[[ a == +(a)]] && echo y
[[ "(a)"]] && echo y
[[ a == a&&(b == b)]] && echo y
[[ ( a == a )]] ; echo $?
EOF
