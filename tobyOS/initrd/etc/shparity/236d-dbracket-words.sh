# [[ ]]: `]]` ends it only as a token of its own (after a blank or a `)`),
# operators are recognised only as unquoted literal words, regexes keep
# their escaped `;` and bracket expressions.
# (batch 12; probe p55 in C:\t\b12)
exec 2>&1
[[ 'x;' =~ x\; ]]; echo "6 $?"
[[ 'x;' =~ x";" ]]; echo "7 $?"
[[ ';' =~ \; ]]; echo "8 $?"
[[ ';' =~ \;$ ]]; echo "9 $?"
