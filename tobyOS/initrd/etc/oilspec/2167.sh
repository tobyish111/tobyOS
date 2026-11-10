$SH <<'EOF'
[[ a =~ [ab] ]] && echo yes
EOF
echo "[ab]=$?"

$SH <<'EOF'
[[ a =~ [a b] ]] && echo yes
EOF
echo "[a b]=$?"

$SH <<'EOF'
[[ a =~ ([a b]) ]] && echo yes
EOF
echo "[a b]=$?"
