# A key read through its quotes inside double quotes: "${A[']']}" and
# "${A["]"]}" are the key `]` in bash, where tsh cut the subscript at the
# first `]` -- a bad substitution (bash's quotearray.tests; finding 288's
# read side, 291).
declare -A A
A[']']=10; A[%]=5; A["]"]=7
echo "${A[']']} ${A[%]}"
echo "${A["]"]}"
echo ${A[']']}
k=']'; echo "${A[$k]}"
echo "${#A[']']}"
