# An arithmetic error names a $'...' as bash's parser leaves it, '...' --
# tsh's message showed the bytes its expander marks the span with
# (finding 569).
exec 2>&1
echo $(( $'12' + 1 ))
echo next
(( $'3' )); echo "rc=$?"
a[$'x\001y']=foo
echo end
