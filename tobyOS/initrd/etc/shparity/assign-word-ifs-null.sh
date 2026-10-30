# The WORD of ${var=WORD} is an assignment's value: with IFS null its $*
# joins with nothing, as `var=$*` does -- "abcdef ghijkl" (finding 395).
# tsh joined with spaces.
exec 2>&1
set -- "abc" "def ghi" "jkl"; A=("$@"); IFS=''
unset var; printf '%s\n' ${var=$*}; printf 'var=%s\n' "$var"
unset var; printf '%s\n' ${var=${A[*]}}; printf 'var=%s\n' "$var"
IFS=:; unset var; printf '%s\n' ${var=$*}; printf 'var=%s\n' "$var"
