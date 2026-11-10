# Inside [[ ]] a `<(` or `>(` is part of the word through its `)`, as the
# lexer reads a process substitution: `ix[7<(4+2)]` is one operand -- tsh
# ended the word at the `<` and called the test a syntax error (finding
# 482).
exec 2>&1
declare -a ix
[[ ix[7<(4+2)] -le 0 ]]; echo "1 $?"
k4='7<(4+2)'
[[ ix[$k4] -le 0 ]]; echo "2 $?"
ix[1]=5
[[ ix[2>(1)] -eq 5 ]]; echo "3 $?"
echo after
