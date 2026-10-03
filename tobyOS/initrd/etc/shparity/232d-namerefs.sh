# Namerefs: ${!ref}, element targets, the checks declare -n and local -n
# make -- a self reference is an error at top level and a warning in a
# function, an empty value still makes the local.
# (batch 12; probe p30 in C:\t\b12)
exec 2>&1
a=('A B' C); typeset -n ra='a[@]'
printf '<%s>' "$ra"; echo " 1"
printf '<%s>' "${ra}"; echo " 2"
printf '<%s>' "x$ra"; echo " 3"
printf '<%s>' "${ra:-d}"; echo " 4"
printf '<%s>' "${ra#A}"; echo " 5"
printf '<%s>' "${ra[@]}"; echo " 6"
printf '<%s>' "${ra@Q}"; echo " 7"
x=$ra; printf '<%s>' "$x"; echo " 8"
typeset -n rs='a[*]'
printf '<%s>' "$rs"; echo " 9"
printf '<%s>' "${rs}"; echo " 10"
printf '<%s>' $ra; echo " 11"
printf '<%s>' ${ra}; echo " 12"
