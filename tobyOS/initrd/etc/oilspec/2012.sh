PS1='foo \d bar'
echo "${PS1@P}" | egrep -q 'foo [A-Z][a-z]+ [A-Z][a-z]+ [0-9]+ bar'
echo matched=$?
