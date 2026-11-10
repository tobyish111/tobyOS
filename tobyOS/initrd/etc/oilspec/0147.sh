declare -A A=(['foo']=bar ['spam']=eggs)
echo declared
b=$(( A ))
echo $b
