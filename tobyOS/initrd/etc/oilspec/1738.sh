var=v
cat <<EOF
var: ${var}
command: $(echo hi)
arith: $((1+2))
EOF
