# [[ ]]: `]]` ends it only as a token of its own (after a blank or a `)`),
# operators are recognised only as unquoted literal words, regexes keep
# their escaped `;` and bracket expressions.
# (batch 12; probe p56 in C:\t\b12)
exec 2>&1
[[ ';' =~ \; ]]; echo "8 $?"
[[ ';' =~ \;  ]]; echo "8b $?"
[[ ';' =~ \;]]; echo "8c $?"
[[ ';' =~ (\;) ]]; echo "8d $?"
x='\;'; [[ ';' =~ $x ]]; echo "8e $?"
[[ 'a;' =~ a\;"" ]]; echo "8f $?"
