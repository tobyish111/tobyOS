declare -A A
A['x']=42
(( x = A['x'] ))
(( A['y'] = 'y' ))  # y is a variable, gets coerced to 0
echo $x ${A['y']}
