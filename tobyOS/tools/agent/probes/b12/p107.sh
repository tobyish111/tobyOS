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
x=$(echo q) y=$(echo q2
echo q3)
echo "L6 next=$LINENO"
