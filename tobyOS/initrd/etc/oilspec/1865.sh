set -- A B
typeset -n ref='@'  # bash gives an error here
echo status=$?

echo ref=$ref  # bash doesn't give an error here
echo status=$?
