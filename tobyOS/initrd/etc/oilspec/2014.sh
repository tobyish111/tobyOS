# In bash y.tab.c uses %X when string is empty
# This doesn't seem to match exactly, but meh for now.

PS1='foo \D{} bar'
echo "${PS1@P}" | egrep -q '^foo [0-9][0-9]:[0-9][0-9]:[0-9][0-9]( ..)? bar$'
echo matched=$?
