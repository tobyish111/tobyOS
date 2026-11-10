# note: modern versions of zsh implement this

array=(1 2 3 '')

test -v 'array[1]'
echo set=$?

test -v 'array[3]'
echo empty=$?

test -v 'array[4]'
echo unset=$?
