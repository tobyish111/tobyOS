# A multi-line ( ) or { } inside $( ): its lines are joined with newlines,
# as the substitution's are (`x=$( (` + newline was a syntax error), and
# $LINENO inside counts bash's printed lines.
# (batch 12; probe p82 in C:\t\b12)
exec 2>&1
x=$( (echo c $LINENO
echo d $LINENO) ); echo $x
eval '(echo e $LINENO
echo f $LINENO)'
(echo g $LINENO
echo h $LINENO)
x=$( { echo i $LINENO
echo j $LINENO; } ); echo $x
x=$(echo k $LINENO
echo l $LINENO); echo $x
x=$( (echo m $LINENO; echo n $LINENO
echo o $LINENO) ); echo $x
