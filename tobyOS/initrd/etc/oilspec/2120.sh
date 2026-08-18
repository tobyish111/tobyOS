tmp="$(basename $SH)-$$.txt"  # unique name for shell and test case
#echo $tmp

stdout_stderr.py &> $tmp

# order is indeterminate
grep STDOUT $tmp
grep STDERR $tmp
