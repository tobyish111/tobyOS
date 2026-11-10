# $LINENO inside a multi-line $( ) counts from the command's own line, and
# blank and comment lines are not counted (bash runs the printed text).
# (batch 12; probe p69 in C:\t\b12)
exec 2>&1
echo A $LINENO \
 $LINENO
echo $(echo B1 $LINENO
echo B2 $LINENO) $LINENO
echo C \
 x $(echo $LINENO
echo $LINENO) $LINENO
echo D \
 $(echo $LINENO) $LINENO
echo \
 $(echo E $LINENO
echo E $LINENO)
x=$(echo F $LINENO
echo F $LINENO) y=$LINENO; echo "$x $y"
echo "G $LINENO
$LINENO" $(echo $LINENO)
echo H $(echo $LINENO
echo $LINENO) $(echo $LINENO
echo $LINENO)
