# $LINENO of a command that spans lines, as bash keeps it: the line on which
# its SECOND token ends -- bash records it when the parser reduces the first
# word, after the lexer has read the next -- and eval's body counts from
# there. Also inside ( ) and { }, whose bodies are copies of the line.
echo "A1
A2" $LINENO
echo x "B1
B2" $LINENO
echo $LINENO "C1
C2"
eval 'echo eval-line1 $LINENO
echo eval-line2 $LINENO'
eval x=1; eval 'echo e3 $LINENO
echo e4 $LINENO'
f() { echo "in f" $LINENO; }; f
: "D1
D2"; echo after-D $LINENO
v=$LINENO"E1
E2"; echo "v=[$v]"
( eval 'echo s1 $LINENO
echo s2 $LINENO' )
{ eval 'echo g1 $LINENO
echo g2 $LINENO'; }
echo end $LINENO
