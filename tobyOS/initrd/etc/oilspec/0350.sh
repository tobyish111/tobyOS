# note: modern versions of zsh implement this

array=(1 2 3)
[[ -v array[1] ]]
echo status=$?

[[ -v array[4] ]]
echo status=$?
