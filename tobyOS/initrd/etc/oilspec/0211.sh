key='\'
declare -A A
#A["$key"]=1

# Works in both
#A["$key"]=42

# Works in bash only
#(( A[\$key] = 42 ))

(( A["$key"] = 42 ))

argv.py "${!A[@]}"
argv.py "${A[@]}"
