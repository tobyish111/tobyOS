type echo > /dev/full
echo status=$?

# other random builtin
ulimit -a > /dev/full
echo status=$?
