a=()
(( a[99]=1 )) # osh doesn't parse index assignment outside arithmetic yet
echo len=${#a[@]}
argv.py "${a[@]}"
echo "unset=${a[33]}"
echo len-of-unset=${#a[33]}
