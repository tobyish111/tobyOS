# An element written by NAME[SUB]=v of an associative array, or by NAME=v of
# an array's element 0, takes the array's -i -l -u as a list's elements do:
# `declare -Au U; U[@]=x` is X (finding 392). tsh stored the text as given.
exec 2>&1
declare -Au U=([k]=low); U[@]=x; U[k]+=y; declare -p U
declare -Ai I; I[a]=2*3; declare -p I
typeset -i n; n=(5+3); n=7*2; declare -p n
declare -al l=(ONE TWO); l=MiXeD; declare -p l
