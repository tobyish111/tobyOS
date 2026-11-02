# A declaration builtin's NAME=VALUE is a word that does not split, not an
# assignment's value: under a null IFS its $* joins with a space, where
# `y=$*` runs the parameters together. tsh ran them together in both
# (finding 559).
exec 2>&1
set -- a 'b c' d; arr=(a 'b c' d)
IFS=
declare y=$*; echo "[$y]"
declare y=${arr[*]} w=${!arr[*]}; echo "[$y][$w]"
f() { local z=$*; echo "[$z]"; }; f a 'b c' d
export e=$*; echo "[$e]"
y=$*; echo "[$y]"
