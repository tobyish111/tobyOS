declare -a array
array+=(a)
array+=(b c)
argv.py "${array[@]}"
