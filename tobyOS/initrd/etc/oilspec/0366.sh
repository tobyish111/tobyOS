export a[7]=8
echo status=$?
argv.py "${!a[@]}" "${a[@]}"
printenv.py a
