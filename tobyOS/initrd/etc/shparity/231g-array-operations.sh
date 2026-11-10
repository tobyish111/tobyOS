# Element operations: negative-index appends, long names, subscripts
# expanded after the value, scalar-as-array slices, "$@" and arrays inside
# ${u:-...} words, assigning a scalar to an array.
# (batch 12; probe p36 in C:\t\b12)
exec 2>&1
set -- '1 2' '3 4'
printf '<%s>' "X${u-x"$@"x}X"; echo " 1"
printf '<%s>' "X${u=x"$@"x}X"; echo " 2 u=[$u]"
printf '<%s>' X${u2-x"$@"x}X; echo " 3"
printf '<%s>' X${u3=x"$@"x}X; echo " 4"
printf '<%s>' "${v+"$@"}"; v=1; printf '<%s>' "${v+"$@"}"; echo " 5"
printf '<%s>' ${v+"$@"} ${v+$@} "${v+$@}"; echo " 6"
printf '<%s>' "${u5:-$*}" "${u5:-"$*"}"; echo " 7"
set --; printf '<%s>' "${v+"$@"}"; echo " 8"
printf '<%s>' "${u6-"$@"}"; echo " 9"
printf '<%s>' "${u7:-${@}}"; echo " 10"
printf '<%s>' x"${u8:-"$@"}"y; echo " 11"
