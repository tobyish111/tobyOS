our_func() { echo ; }
our_func2() { echo ; }
alias our_alias=foo

compgen -A command our_
echo status=$?

# Introduce another function.  Note that we're missing test coverage for
# 'complete', i.e. bug #1064.
our_func3() { echo ; }

compgen -A command our_
echo status=$?
