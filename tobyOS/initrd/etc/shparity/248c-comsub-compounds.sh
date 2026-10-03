# A multi-line ( ) or { } inside $( ): its lines are joined with newlines,
# as the substitution's are (`x=$( (` + newline was a syntax error), and
# $LINENO inside counts bash's printed lines.
# (batch 12; probe p84 in C:\t\b12)
exec 2>&1
(
echo x $LINENO
)
{
echo y $LINENO
}
x=$( (
echo z $LINENO
) ); echo "$x"
x=$({
echo w $LINENO
}); echo "$x"
