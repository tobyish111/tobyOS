PS1='[\u0001]'
USER=$(whoami)
test "${PS1@P}" = "[${USER}0001]"
echo status=$?
