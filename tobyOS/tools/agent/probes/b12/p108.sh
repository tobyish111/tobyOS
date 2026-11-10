x=$(echo q) y="q2
q3" z=$LINENO; echo "V1 z=$z"
echo $(echo q) $(echo q2
echo q3) $LINENO
echo q $(echo q2
echo q3) $LINENO
x=1 y=$(echo q2
echo q3) z=$LINENO; echo "V4 z=$z"
echo "q" "q2
q3" $LINENO
echo q `echo q2
echo q3` $LINENO
echo q $((1+
2)) $LINENO
echo q ${x:-a
b} $LINENO
echo q <(true
) >/dev/null; echo "V9 $LINENO"
echo $(echo a
) q $LINENO
