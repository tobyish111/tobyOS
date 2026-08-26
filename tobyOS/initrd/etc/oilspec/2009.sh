PS1='foo \T bar'
echo "${PS1@P}" | egrep -q 'foo [0-1][0-9]:[0-5][0-9]:[0-5][0-9] bar'
echo matched=$?
