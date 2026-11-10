exec 2>&1
echo "a $(echo $LINENO
echo $LINENO)" | tr '\n' ' '; echo
echo b $(echo $LINENO
echo $LINENO)
x=$(echo $LINENO
echo $LINENO); echo "c $x" | tr '\n' ' '; echo
echo d "$(
echo $LINENO
echo $LINENO
)" | tr '\n' ' '; echo
echo e $( (echo $LINENO
echo $LINENO) )
echo f $( { echo $LINENO
echo $LINENO; } )
echo g $(
echo $LINENO)
echo h $(: x
echo $LINENO)
echo i $(true

echo $LINENO)
echo j $(echo $LINENO; echo $LINENO
echo $LINENO)
echo k \
 $(echo $LINENO
echo $LINENO)
echo l $(echo $LINENO
echo $LINENO
echo $LINENO
echo $LINENO)
echo m `echo $LINENO
echo $LINENO`
echo n "`echo $LINENO
echo $LINENO`"
echo o $LINENO
