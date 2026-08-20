# $LINENO -- the SOURCE line, not a count of logical lines.
#
# The reader joins a compound's lines into one before anything parses it,
# so a counter that ticked once per logical line reported the line the
# compound STARTED on for every command in its body, and was short by the
# number of lines it swallowed for everything after it.
echo A=$LINENO
echo B=$LINENO
set -- a b c
for x; do
  echo C=$LINENO $x
done
echo D=$LINENO
if true; then
  echo E=$LINENO
fi
echo F=$LINENO
n=1
while (( n-- )); do
  echo G=$LINENO
done
echo H=$LINENO
echo done
