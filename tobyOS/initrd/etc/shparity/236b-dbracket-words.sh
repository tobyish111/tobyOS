# [[ ]]: `]]` ends it only as a token of its own (after a blank or a `)`),
# operators are recognised only as unquoted literal words, regexes keep
# their escaped `;` and bracket expressions.
# (batch 12; probe p53 in C:\t\b12)
exec 2>&1
s='(hi)'
[[ "$s" =~ ^(;[^x]*) ]]; echo "1 $?"
[[ "$s" =~ ^(;[^$'\n']*) ]]; echo "2 $?"
[[ "$s" =~ ^([~\'\`]) ]]; echo "3 $?"
[[ "$s" =~ ^^([][{}\(\)^@]) ]]; echo "4 $? ${BASH_REMATCH[0]}"
[[ "$s" =~ (\"(\\.|[^\\\"])*\") ]]; echo "5 $?"
[[ "$s" =~ ^([^][ ~\`\'\";{}\(\)^@\,]+) ]]; echo "6 $?"
[[ "$s" =~ ^[,]|^[[:space:]]+ ]]; echo "7 $?"
[[ a =~ a|b ]]; echo "8 $?"
[[ a =~ (a;b) ]]; echo "9 $?"
[[ a =~ a\ b ]]; echo "10 $?"
[[ "a b" =~ a\ b ]]; echo "11 $?"
[[ ';' =~ ^\; ]]; echo "12 $?"
