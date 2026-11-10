shopt -s strict_arith || true
s=foo
echo $s
echo $((s+5))
echo 'should not get here'
