# $(( )) that is really $( ( ... ) ), `${ cmd; }` as a bad substitution, and
# arithmetic across lines.
# (batch 12; probe p64 in C:\t\b12)
exec 2>&1
echo $((1
+ 2))
x=$(( 3 *
  4 ))
echo "x=$x $LINENO"
echo "$(( 5
- 1 ))"
y=$((
10
))
echo "y=$y $LINENO"
echo $((1 +
)) ; echo "st=$?"
echo "after $LINENO"
echo $(( echo 1
echo 2
(( x ))
: $(( x ))
echo 3
) )
echo "end $LINENO"
a[$((1
+1))]=z; echo "${a[2]}"
