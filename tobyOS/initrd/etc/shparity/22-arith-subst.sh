# GAP 8: expansions inside $(( )).
# The evaluator resolves bare variable names itself but knew nothing about
# $( ) or ${ }, so anything richer than `n + 1` died as "bad arithmetic
# expansion". The expression text is now expanded before it is evaluated.
echo $(( $(echo 4) + 1 ))
echo $(( $(echo 2) ** $(echo 3) ))
echo $(( (1 + 2) * $(echo 3) ))

n=7
echo $(( n * 2 ))
echo $(( $n + 3 ))

s=abcde
echo $(( ${#s} + 1 ))

i=0
i=$(( i + $(echo 5) ))
echo "i=$i"
i=$(( $i * 2 ))
echo "i=$i"

echo $(( 10 / $(echo 2) ))
echo $(( $(echo 9) % 4 ))
a=3
b=4
echo $(( a * a + b * b ))
echo "nested=$(( $(( 2 + 3 )) * 2 ))"
echo arithsubst-done
