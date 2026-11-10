# In a function, local or declare of NAME[SUB]=v where NAME is the
# function's own reference makes the reference's target a fresh local and
# assigns the element there; the global target is left as it was. tsh wrote
# the global's element (finding 574).
exec 2>&1
f() { local -n x=arr; local -a x[1]=one; echo "rc=$?"; declare -p x arr; }
arr=(a); f; declare -p arr
g() { local -n x=arr; declare x[2]=two; declare -p arr; }; g; declare -p arr
h() { local -n x=m; local -A x[k]=v; declare -p m; }; declare -A m=([j]=1); h; declare -p m
