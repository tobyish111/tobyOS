declare -A A=(['foo']=42)
key='foo'

# note: in bash, (( A[\$key] += 1 )) works the same way.

set -- 1 2
(( A[$key] += $2 ))

echo foo=${A['foo']}
