# An associative key in arithmetic is a word: (( x = A['x'] )) reads key x;
# (( b = a )) reads element 0 of an array; unset of an element of a SCALAR
# unsets it at index 0 and is "not an array variable" otherwise; an
# associative key in unset is expanded, quotes and all.
exec 2>&1
declare -A A; A['x']=42; (( q = A['x'] )); echo q=$q
(( A['y'] = 7 )); echo ${A[y]}
a=(4 5 6); (( b = a )); echo b=$b
declare undef; unset -v 'undef[1]'; echo st=$?; unset -v 'undef["key"]'; echo st=$?
declare -A dict=(); key='1],a[1'; dict["$key"]=foo; unset -v 'dict["$key"]'; echo n=${#dict[@]}
