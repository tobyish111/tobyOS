array=(1 2 3 '')

test -v 'array[1+1]'
echo status=$?

test -v 'array[4+1]'
echo status=$?

echo
echo dbracket

[[ -v array[1+1] ]]
echo status=$?

[[ -v array[4+1] ]]
echo status=$?
