declare -a array
array=(zero one two three)

echo ${array[1+2]}

code='1+2'
echo ${array[$code]}


# it still dynamically parses
