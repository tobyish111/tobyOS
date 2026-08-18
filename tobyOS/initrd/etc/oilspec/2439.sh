declare -a a=({1..9})
declare -A A=(['a']=hello ['b']=world ['c']=osh ['d']=ysh)

argv.py "${a[@]@P}"
argv.py "${a[*]@P}"
argv.py "${A[@]@P}"
argv.py "${A[*]@P}"
argv.py "${u[@]@P}"
argv.py "${u[*]@P}"
