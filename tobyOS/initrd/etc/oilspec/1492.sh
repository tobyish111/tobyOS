declare -A A=(['foo']=bar ['spam']=42)
(( x = A['spam'] ))
echo $x
