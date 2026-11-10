PS1='\u '
USER=$(whoami)
test "${PS1@P}" = "${USER} "
echo status=$?
