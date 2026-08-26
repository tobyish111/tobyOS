PS1='foo \D{%H:%M} bar'
echo "${PS1@P}" | egrep -q 'foo [0-9][0-9]:[0-9][0-9] bar'
echo matched=$?

PS1='foo \D{%H:%M:%S} bar'
echo "${PS1@P}" | egrep -q 'foo [0-9][0-9]:[0-9][0-9]:[0-9][0-9] bar'
echo matched=$?
