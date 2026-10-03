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
