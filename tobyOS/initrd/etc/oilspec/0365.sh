readonly b[7]=8
echo status=$?
argv.py "${!b[@]}" "${b[@]}"

# bash doesn't like this variable name!
