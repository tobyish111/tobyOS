PS1='foo \@ bar'
echo "${PS1@P}" | egrep -q 'foo [0-1][0-9]:[0-5][0-9] (A|P)M bar'
echo matched=$?
