# a3.sh: the findings audit, the grammar check -- 35 (a $( ) in a
# syntax error's token), 36 (a trap action's syntax error), 37 (a $( )
# ending inside a here-document), and 40 (a redirection's access mode).
exec 2>&1
N() { sed 's/^[^:]*: //'; }
$SH -c 'echo $( a|b ) )' 2>&1 | N
$SH -c 'cat <( sh) )' 2>&1 | N
$SH -c 'trap "if then" EXIT; echo body' 2>&1 | N
$SH -c 'trap "echo (" EXIT; echo body2' 2>&1 | N
$SH -c 'trap "fi" DEBUG; echo dbg' 2>&1 | N
$SH -c 'trap "done" ERR; false; echo err' 2>&1 | N
$SH -c 'x=$(cat <<EOF)
hello
EOF
echo "[$x]"' 2>&1 | N
$SH -c 'echo "[$(cat <<EOF
a
EOF
)]"' 2>&1 | N
cd /tmp; echo data > r40
exec 3<r40; echo x >&3; echo "f40a $?"; exec 3<&-
cat r40
exec 3>r40; cat <&3; echo "f40b $?"; exec 3>&-
rm -f r40
echo end
