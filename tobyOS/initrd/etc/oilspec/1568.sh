# We have to capture stderr here 

filter_err() {
  # check for bash/dash/mksh messages, and unwanted Python OverflowError
  egrep -o 'Illegal number|bad number|return: can only|expected a small integer|OverflowError'
  return 0
}

# true; disables subshell optimization!

# exit status too big, but integer isn't
$SH -c 'true; ( return 2147483647; )' 2>err.txt
echo status=$?
cat err.txt | filter_err

# now integer is too big
$SH -c 'true; ( return 2147483648; )' 2> err.txt
echo status=$?
cat err.txt | filter_err

# even bigger
$SH -c 'true; ( return 2147483649; )' 2> err.txt
echo status=$?
cat err.txt | filter_err

echo
echo '--- negative ---'

# negative vlaues
$SH -c 'true; ( return -2147483648; )' 2>err.txt
echo status=$?
cat err.txt | filter_err

# negative vlaues
$SH -c 'true; ( return -2147483649; )' 2>err.txt
echo status=$?
cat err.txt | filter_err


# osh-cpp checks overflow, but osh-py doesn't


# mksh behaves similarly, uses '1' as its "bad status" status!


# dash is similar, but seems to reject negative numbers


# bash disallows return at top level
