# Command substitution in both spellings, nested, and inside arithmetic.
# Trailing newlines are stripped by $(...) -- the multi-line case below is
# what catches a shell that forgets to strip only the TRAILING ones.
x=$(echo inner)
echo "x=$x"
echo "direct=$(echo nested $(echo deep))"
y=`echo backtick`
echo "y=$y"
echo "arith=$(( $(echo 4) + 1 ))"
multi=$(echo a; echo b)
echo "multi=[$multi]"
echo "unquoted=[$multi]" | while read l; do echo "$l"; done
empty=$(true)
echo "empty=[$empty]"
n=$(echo one; echo two; echo three)
echo "lines=[$n]"
