# All of them give errors:
# dash - bad fd number, parse error?
# bash - ambiguous redirect
# mksh - illegal file descriptor name
set -- '2 3' 'c d'
echo hi 1>& "$@"
