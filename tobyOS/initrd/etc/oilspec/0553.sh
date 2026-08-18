# here it's equality
test '(' = ')'
echo status=$?

# here it's like -n =
test 0 -eq 0 -a '(' = ')'
echo status=$?
