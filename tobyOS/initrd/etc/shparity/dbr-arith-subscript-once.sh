# An arithmetic operand of [[ ]] written NAME[...] has its subscript
# expanded once, as (( )) does: a key holding `]` or `$( )` is the key --
# tsh expanded the word, so the evaluator met `as[]]` and ran the `$( )`
# (finding 483).
exec 2>&1
declare -A as
k='p],q[$(echo ran >&2)'
k1=']'
as[$k]=42; as[$k1]=12; as[0]=0
[[ as[$k] -eq as[$k] ]]; echo "1 $?"
[[ as[$k] -gt as[$k1] ]]; echo "2 $?"
[[ as[$k1] -lt 13 ]]; echo "3 $?"
[[ as["$k1"] -eq 12 ]]; echo "4 $?"
v='as[0]'
[[ $v -eq 0 ]]; echo "5 $?"
