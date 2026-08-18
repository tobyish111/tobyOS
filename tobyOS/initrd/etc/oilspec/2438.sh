declare -a a=({1..9})
declare -A A=(['a']=hello ['b']=world ['c']=osh ['d']=ysh)

argv.py "${a[@]@Q}"
argv.py "${a[*]@Q}"
argv.py "${A[@]@Q}"
argv.py "${A[*]@Q}"
argv.py "${u[@]@Q}"
argv.py "${u[*]@Q}"
