# [[ ]]: `]]` ends it only as a token of its own (after a blank or a `)`),
# operators are recognised only as unquoted literal words, regexes keep
# their escaped `;` and bracket expressions.
# (batch 12; probe p62 in C:\t\b12)
exec 2>&1
eval '[[ -n == ]]'; echo "a $?"
eval '[[ -f == ]]'; echo "b $?"
eval '[[ -n == ]] && echo y'; echo "c $?"
eval '[[ -n -eq ]]'; echo "d $?"
eval '[[ -n ! ]]'; echo "e $?"
eval '[[ -f -f ]]'; echo "f $?"
eval '[[ -n x ]] && echo y'; echo "g $?"
eval '[[ x\-f == x-f ]] && echo t9'; echo "h $?"
eval '[[ \-f x ]]'; echo "s10 $?"
eval '[[ "-f" x ]]'; echo "s11 $?"
eval '[[ -f"" x ]]'; echo "s12 $?"
eval '[[ -n == && -n x ]]'; echo "s13 $?"
eval '[[ -n =~ ]]'; echo "s14 $?"
eval '[[ -n -nt ]]'; echo "s15 $?"
