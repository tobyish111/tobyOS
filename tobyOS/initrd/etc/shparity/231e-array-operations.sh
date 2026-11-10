# Element operations: negative-index appends, long names, subscripts
# expanded after the value, scalar-as-array slices, "$@" and arrays inside
# ${u:-...} words, assigning a scalar to an array.
# (batch 12; probe p34 in C:\t\b12)
exec 2>&1
s=abcd
printf '<%s>' "${s[@]:0}" "${s[@]:1}" "${s[@]: -1}" "${s[@]:0:1}"; echo " 1"
printf '<%s>' "${!s[@]}" "${#s[@]}" "${s[@]#a}" "${s[*]/b/X}" "${s[@]@Q}" "${s[@]^^}"; echo " 2"
printf '<%s>' "${s[@]:-d}" "${s[@]+p}" "${#s[*]}" "${!s[*]}"; echo " 3"
e=; printf '<%s>' "${e[@]}" "${#e[@]}" "${e[@]:-d}" "${e[@]-d}"; echo " 4"
declare u; printf '<%s>' "${u[@]}" "${#u[@]}" "${u[@]-d}"; echo " 5"
IFS=:; t='a b'; printf '<%s>' ${t[@]} ${t[*]}; echo " 6"
