# An escaped or quoted IFS character in a word or an array literal is data,
# not a delimiter: `IFS=: a=(x\:y)` is one element. tsh split it.
# (batch 12; probe p1 in C:\t\b12)
exec 2>&1
IFS=':%'
a=(x\:y); echo "${#a[@]} ${a[0]}"
a=(ham\ cheese\:colon); echo "${#a[@]} ${a[0]}"
a=('p:q' "r:s"); echo "${#a[@]} ${a[0]} ${a[1]}"
set -- x\:y; echo "$# $1"
for w in x\:y; do echo "for $w"; done
