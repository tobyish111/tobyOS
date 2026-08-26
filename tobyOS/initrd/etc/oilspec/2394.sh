declare s
s='1 '
s+=' 2 '  # string append

declare -i i
i='1 '
i+=' 2 '  # arith add

declare -i j
j=x  # treated like zero
j+=' 2 '  # arith add

echo "[$s]"
echo [$i]
echo [$j]
