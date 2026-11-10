# Unquoted under a null IFS, ${*OP} and ${a[*]OP} are a field per element,
# as $* and ${a[*]} are there -- tsh joined them into one (finding 493).
# (bash joins ${a[*]#...} with a space even so; that one is left out.)
exec 2>&1
set -- ' A ' ' B '
IFS=
printf '<%s>' ${*} ${*##} ${*/} ${*,,} ${*:1:2} ${*@Q}; echo
printf '<%s>' ${*%} ${*/A/a} ${*^} ${*:0:1} ${*@U}; echo
printf '<%s>' "${*##}" "${*,,}"; echo
a=(' C ' ' D ')
printf '<%s>' ${a[*]%} ${a[*]/C/c} ${a[*],,} ${a[*]:1} ${a[*]@Q}; echo
unset IFS
printf '<%s>' ${*##} ${a[*]^^}; echo
