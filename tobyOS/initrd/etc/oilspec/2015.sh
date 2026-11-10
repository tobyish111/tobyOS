PS1='foo \s bar'
echo "${PS1@P}" | egrep -q '^foo (bash|osh) bar$'
echo match=$?

PS1='foo \v bar'
echo "${PS1@P}" | egrep -q '^foo [0-9]+\.[0-9]+ bar$'
echo match=$?

PS1='foo \V bar'
echo "${PS1@P}" | egrep -q '^foo [0-9]+\.[0-9]+\.[0-9]+ bar$'
echo match=$?
