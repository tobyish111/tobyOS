test '(' x ')'
echo status=$?

test 0 -eq 0 -a '(' x ')'
echo status=$?
