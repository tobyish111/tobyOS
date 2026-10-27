# An assignment's subscript is read through its quotes: `A[']']=10` closed
# at the quoted `]`, the `'` after it opened a string, and the reader ran
# to the end of the script (bash's quotearray.tests).
declare -A A
A[']']=10
A[%]=5
A["]"]=11
A[$'k\'']=12
declare -p A
echo after
