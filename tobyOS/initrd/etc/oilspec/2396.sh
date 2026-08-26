declare s
(( s='1 '))
(( s+=' 2 '))  # arith add
declare -i i
(( i='1 ' ))
(( i+=' 2 ' ))
declare -i j
(( j='x ' ))  # treated like zero
(( j+=' 2 ' ))
echo "$s|$i|$j"
