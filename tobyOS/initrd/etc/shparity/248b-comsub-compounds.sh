# A multi-line ( ) or { } inside $( ): its lines are joined with newlines,
# as the substitution's are (`x=$( (` + newline was a syntax error), and
# $LINENO inside counts bash's printed lines.
# (batch 12; probe p83 in C:\t\b12)
exec 2>&1
x=$(:; (echo c $LINENO
echo d $LINENO) ); echo $x
x=$( (echo e $LINENO
echo f $LINENO); : ); echo $x
x=$(eval '(echo g $LINENO
echo h $LINENO)'); echo $x
x=$( (
echo i $LINENO
echo j $LINENO
) ); echo $x
x=$( while :; do echo k $LINENO
echo l $LINENO; break; done ); echo $x
