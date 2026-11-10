exec 2>&1
(echo a $LINENO
echo b $LINENO)
x=$( (echo c $LINENO
echo d $LINENO) ); echo $x
{ echo e $LINENO
echo f $LINENO; }
x=$( { echo g $LINENO
echo h $LINENO; } ); echo $x
x=$(echo i $LINENO
(echo j $LINENO
echo k $LINENO)); echo $x
x=$(if true; then echo l $LINENO
echo m $LINENO; fi); echo $x
