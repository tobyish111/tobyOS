echo $((a + x42))
echo status=$?

# weird asymmetry -- the above is a syntax error, but this isn't
$SH -c 'echo $((a + 42x))'
echo status=$?

# regression
echo $((a + 42x))
echo status=$?
