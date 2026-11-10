array=(X Y Z)
typeset -n ref='array[0]'
ref[0]=foo  # error in bash: 'array[0]': not a valid identifier
echo status=$?
echo ${array[@]}
