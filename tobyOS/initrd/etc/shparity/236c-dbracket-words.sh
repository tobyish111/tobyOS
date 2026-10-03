# [[ ]]: `]]` ends it only as a token of its own (after a blank or a `)`),
# operators are recognised only as unquoted literal words, regexes keep
# their escaped `;` and bracket expressions.
# (batch 12; probe p54 in C:\t\b12)
exec 2>&1
[[ ';' =~ ^\; ]]; echo "a $?"
[[ ';' =~ \; ]]
echo "b $?"
x='^\;'; [[ ';' =~ $x ]]; echo "c $?"
[[ 'a;' =~ a\; ]] && echo "d yes"
[[ 'x' =~ [[:alpha:]] ]]; echo "e $?"
[[ 'x' =~ ^[[:alpha:]]+$ ]]; echo "f $?"
[[ ' ' =~ ^[,]|^[[:space:]]+ ]]; echo "g $?"
