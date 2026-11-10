shopt -u strict_arith || true
(( undef1++ ))
(( ++undef2 ))
echo "[$undef1][$undef2]"
