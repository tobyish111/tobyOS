declare -a arr=( [30]=a b [40]=x y)
argv.py "${!arr[@]}"
argv.py "${arr[@]}"
