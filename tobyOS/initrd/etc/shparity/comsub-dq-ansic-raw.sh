# A $( ) runs the text bash's reader kept, and there a $'...' inside a
# double-quoted ${ } is the string it stands for, not quoted again: $'o\'k'
# leaves a quote open, so the text is "command substitution: line N:
# unexpected EOF ...", none of it runs and the rest of the line goes, a
# function's too. tsh ran the text as written and said "bad substitution"
# (finding 616).
exec 2>&1
x=$(echo ran; echo "${zz-$'o\'k'}"); echo "same line"
echo "rc=$? [$x]"
x=$(echo "${zz-$'o\"k'}")
echo "rc=$?"
x=$(echo "${zz-$'a\tb'}"); echo "ok [$x]"
f() { x=$(echo "${zz-$'o\'k'}"); echo "in f"; }
f; echo "after f"
echo end
