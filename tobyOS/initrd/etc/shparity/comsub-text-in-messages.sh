# bash 5.2 reads a $( ) as a command and keeps it in the word as its
# command printer prints it -- which is what it runs. So a message that names
# a word unexpanded shows the printed form: `>&2` as `1>&2`, `a|b` as
# `a | b`, a compound laid out on its lines, a comment gone; at any depth,
# in "" and ${ } and $(( )); a backquote as written. tsh named the word as
# it was typed (finding 645).
exec 2>&1
declare -a arr
arr[$(echo I >&2;echo 1)]=v echo w1
arr[$(echo   a|cat;echo  b)]=v echo w2
arr[$(true&&echo a||echo b>/dev/null)]=v echo w3
arr[$(if true;then echo a;fi)]=v echo w4
arr[${x:-$(echo   a >&2)}]=v echo w5
arr["$(echo   a >&2)"]=v echo w6
arr[$(( $(echo   1) + 1 ))]=v echo w7
arr[$(echo $(echo   n >&2))]=v echo w8
arr[$( (echo  a) )]=v echo w9
arr[<(echo   p)]=v echo w10
arr[`echo   b >&2`]=v echo w11
arr[$(echo a # note
)]=v echo w12
echo hi > $(echo   x  y;true); echo "ambiguous: $?"
cat < ${u:-$(echo   a  b)}; echo "ambiguous in \${ }: $?"
echo "${x$(echo   a >&2)}"; echo "bad substitution: $?"
echo "${x$(echo   a)${y:-$(echo   b)}}"; echo "nested: $?"
( set -u; b=(1); echo "${b[$(echo   2)+$(echo  0)]}" ); echo "unbound: $?"
