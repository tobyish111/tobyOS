# A simple command's line is where bash's parser reduced its first word:
# a WORD needs one token of lookahead (it might be `name ()`), an
# ASSIGNMENT does not -- `x=1 y=$(...<newline>...) z=$LINENO` is the first
# line, and tsh said the second. And a process substitution may go on to
# the next line: tsh said "unterminated process substitution".
# (batch 12; probes p107 p108 in C:\t\b12)
exec 2>&1
: "$(echo a)"; echo "L1=$LINENO"
x=$(echo q) y=$(echo q2
echo q3) z=$LINENO; echo "L2 z=$z"
x=$(echo b
echo b2) z=$LINENO; echo "L3 z=$z"
echo "A" $(echo b
echo c) $LINENO
x=$(
echo p
) z=$LINENO; echo "L5 z=$z"
x=$(echo q) y="q2
q3" z=$LINENO; echo "V1 z=$z"
x=1 y=$(echo q2
echo q3) z=$LINENO; echo "V4 z=$z"
echo "q" "q2
q3" $LINENO
echo q <(true
) >/dev/null; echo "V9 $LINENO"
cat <(echo one
echo two)
echo "next=$LINENO"
