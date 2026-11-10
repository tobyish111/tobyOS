# test's errors in bash's words, where tsh said "syntax error near 'X'"
# for all of them (bash's test.tests; finding 291).
t() { test "$@" 2>&1; echo "rc=$?"; }
t a b
t -A v
t 1 -ne
t 1 2 3 4
t '(' 1 -eq 2
[ '(' 1 -eq 2 ] 2>&1; echo "rc=$?"
t ! a b
t a b c
t '(' a b c d
t a -a '(' b c
t 1 -eq 2 -o
t a = a -x b
t a -o b -
t -z -z -z -z -z
t 1 -lt 2 -a 3 -eq
t a = b c
