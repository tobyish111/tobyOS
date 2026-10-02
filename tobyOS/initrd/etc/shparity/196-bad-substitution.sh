# "${TEXT}: bad substitution": bash names the whole expression, drops the
# rest of the line and goes on with status 1 -- except an unknown @ operator,
# which ends the script. And $[ EXPR ] is bash's old spelling of $(( )).
exec 2>&1
echo ${%}; echo n
echo "a2 $?"
echo ${a&}; echo n
echo "a3 $?"
x=abc; echo ${#x-default}; echo n
echo "a4 $?"
echo ${x:}; echo n
echo "a7 $?"
a=(1 2); echo ${a[0][0]}; echo n
echo "a8 $?"
echo ${#a[0]/1/xxx}; echo n
echo "a11 $?"
y=3; echo $[1+2] $[y*2] "$[y]" $[ $y + 1 ] $[undefined]
echo ${x@Z}; echo n
echo "not reached"
