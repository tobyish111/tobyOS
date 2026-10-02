# $LINENO of a simple command that spans lines. bash 5.2 records a simple
# command's line when its parser reduces the FIRST word -- after the lexer
# has read the next token -- so the line is where that second token ends.
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
echo end $LINENO
