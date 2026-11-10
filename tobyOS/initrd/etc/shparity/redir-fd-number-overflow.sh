# Digits right before < or > are a descriptor only when the number fits an
# int: 1111111111111111111111</dev/stdin runs a command of that name, where
# tsh's count overflowed into some descriptor (finding 406).
exec 2>&1
{ echo "[$(1111111111111111111111</dev/stdin)]"; } <<<'hi'; echo "rc=$?"
2147483648</dev/null; echo "rc=$?"
echo ok 3>/dev/null
