# [[ ]]: `]]` ends it only as a token of its own (after a blank or a `)`),
# operators are recognised only as unquoted literal words, regexes keep
# their escaped `;` and bracket expressions.
# (batch 12; probe p60 in C:\t\b12)
exec 2>&1
[[ ''||! (1 == 2)&&(2 == 2)]] && echo t1
[[ (a == a)]] && echo t2
eval '[[ " " =~ [[:space:]]]]' && echo t3; echo "s3 $?"
eval '[[ a == a]]' && echo t4; echo "s4 $?"
eval '[[ a == a ]]' && echo t5
eval '[[ " " =~ [[:space:]] ]]' && echo t6
eval '[[ a == @(a|b)]]' && echo t7; echo "s7 $?"
eval '[[ -n x&&-n y]]' && echo t8; echo "s8 $?"
eval '[[ ! a]]' && echo t9; echo "s9 $?"
eval '[[ a =~ a]]' && echo t10; echo "s10 $?"
eval '[[ a]] ]]' && echo t11; echo "s11 $?"
