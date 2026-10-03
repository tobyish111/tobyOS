B=/home/toby/bash-5.2.15/bash
while IFS= read -r line; do
  out=$($B -c "$line" 2>&1); rc=$?; echo "[$line] => rc=$rc out=$(echo $out)"
done <<'EOF'
false; [[ "-f" x ]]
true; [[ "-f" x ]]
false; [[ a "==" a ]]
false; [[ a b c ]]
false; [[ -n x y ]]
false; [[ ( a ]]
false; [[ a == b c ]]
EOF
