set -- 'a b' 'c'
array1=('x y' 'z')
array2=("$@")
argv.py "${array1[@]}" "${array2[@]}"
