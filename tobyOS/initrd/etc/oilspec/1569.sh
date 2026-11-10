# We have to capture stderr here 

filter_err() {
  # check for bash/dash/mksh messages, and unwanted Python OverflowError
  egrep -o 'Illegal number|bad number|return: can only|expected a small integer|OverflowError'
  return 0
}

# exit status too big, but integer isn't
$SH -c 'f() ( return 2147483647; ); f' 2>err.txt
echo status=$?
cat err.txt | filter_err

# now integer is too big
$SH -c 'f() ( return 2147483648; ); f' 2> err.txt
echo status=$?
cat err.txt | filter_err

# even bigger
$SH -c 'f() ( return 2147483649; ); f' 2> err.txt
echo status=$?
cat err.txt | filter_err



# bash truncates it to 0 here, I guess it's using 64 bit integers



# Weird case from bash-help mailing list.
#
# "Evaluations of backticks in if statements".  It doesn't relate to if
# statements but to $?, since && and || behave the same way.

# POSIX has a special rule for this.  In OSH strict_argv is preferred so it
# becomes a moot point.  I think this is an artifact of the
# "stateful"/imperative nature of $? -- it can be "left over" from a prior
# command, and sometimes the prior argv is [].  OSH has a more "functional"
# implementation so it doesn't have this weirdness.
