PS1='\W '
test "${PS1@P}" = "$(basename $PWD) "
echo status=$?
