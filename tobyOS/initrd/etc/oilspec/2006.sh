PS1='\w '
test "${PS1@P}" = "${PWD} "
echo status=$?
