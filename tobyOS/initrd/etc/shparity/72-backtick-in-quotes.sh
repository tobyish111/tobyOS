# A BACKTICK INSIDE DOUBLE QUOTES IS A SEPARATE LEXICAL WORLD. bash removes
# one layer of backslashes inside `` before parsing what is left, so a `\`
# in there is ONE backslash and the `"` after it is escaped rather than the
# end of the string. Tracking quotes straight through reached the opposite
# conclusion and the line was never judged complete.
echo "a `echo hi` b"
echo "a `[[ 1 -eq 1 ]]` b"
echo "a `[[ $(echo x) ]]` b"

f=/tmp/bt-quote
echo "123 `[[ $(echo \\" > $f) ]]` 456"
cat $f
echo done
