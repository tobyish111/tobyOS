PS1='foo \t bar'
echo "${PS1@P}" | egrep -q 'foo [0-2][0-9]:[0-5][0-9]:[0-5][0-9] bar'
echo matched=$?
