# `(( expr ))`, the arithmetic command.
#
# `$(( ))` -- the EXPANSION -- had worked for a long time; the COMMAND form
# fell through to the subshell parser, which saw `( ( a = 42 ) )` and tried
# to run `a = 42` as a program. The status is INVERTED relative to the
# value, like test: a non-zero result is success, which is what makes
# `while (( n-- ))` terminate.
(( a = 42 ))
echo A=$? a=$a
(( 1 ))
echo B=$?
(( 0 ))
echo C=$?
(( a > 10 ))
echo D=$?
(( a < 10 ))
echo E=$?
n=3
while (( n-- )); do echo F=$n; done
if (( a == 42 )); then echo G=yes; fi
(( b = a * 2 ))
echo H=$b
(( c = (1 + 2) * 3 ))
echo I=$c
echo done
