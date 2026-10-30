# declare NAME[SUB]=VALUE of a reference works on the reference's own name:
# "removing nameref attribute", and NAME is the array -- with a target or
# without, with -a, -A or neither -- the target untouched (finding 358).
exec 2>&1
declare -n x; declare -a x[1]=one; echo "rc=$?"; declare -p x
declare -n y=arr; declare y[2]=two; declare -p y; declare -p arr; echo "rc=$?"
declare -n z=arr2; declare -A z[k]=v; declare -p z
