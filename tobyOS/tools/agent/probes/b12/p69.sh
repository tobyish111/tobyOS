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
