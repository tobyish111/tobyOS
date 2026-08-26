set -o history # enable history
PS1='foo \! bar'
history -c # clear history
echo "${PS1@P}" | egrep -q "foo 1 bar"
echo matched=$?
echo "_${PS1@P}" | egrep -q "foo 3 bar"
echo matched=$?
