declare -A A  # still undefined
argv.py "${A[@]}"
argv.py "${!A[@]}"
A['foo']=bar
echo ${A['foo']}
