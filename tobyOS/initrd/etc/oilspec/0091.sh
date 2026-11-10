x='1'
echo $(( x + 2 * 3 ))
echo status=$?

# Expression like values are evaluated first (this is unlike double quotes)
x='1 + 2'
echo $(( x * 3 ))
echo status=$?
