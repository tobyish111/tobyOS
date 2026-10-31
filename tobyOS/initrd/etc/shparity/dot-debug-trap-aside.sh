# Without functrace a sourced file runs without the DEBUG trap, which comes
# back after it and its RETURN trap -- unless the file set one of its own;
# tsh ran the trap in the file (finding 534).
exec 2>&1
printf '%s\n' 'echo one' 'echo two' > d1.sub
printf '%s\n' 'trap -p DEBUG' "trap 'echo \"N \$LINENO\"' DEBUG" 'echo inside' > d2.sub
printf '%s\n' 'trap - DEBUG' 'echo inside' > d3.sub
trap 'echo "D $LINENO"' DEBUG
trap 'echo "R $LINENO"' RETURN
. ./d1.sub
. ./d3.sub
. ./d2.sub
echo after
trap - DEBUG RETURN
set -T
trap 'echo "T $LINENO"' DEBUG
. ./d1.sub
set +T
trap - DEBUG
