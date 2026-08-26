declare -a a=({1..9})
declare -A A=(['a']=hello ['b']=world ['c']=osh ['d']=ysh)

argv.py "${a[@]@a}"
argv.py "${a[*]@a}"
argv.py "${A[@]@a}"
argv.py "${A[*]@a}"
argv.py "${u[@]@a}"
argv.py "${u[*]@a}"
