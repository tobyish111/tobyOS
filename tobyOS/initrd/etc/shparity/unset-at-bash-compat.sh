# Under BASH_COMPAT 5.1 or lower, `unset NAME[@]` removes the whole
# variable, indexed or associative; under 5.2 an indexed one is emptied and
# an associative one loses the key `@` -- tsh ignored BASH_COMPAT (finding
# 485).
exec 2>&1
declare -a ar=(1 2 3)
unset ar[@]
declare -p ar
declare -A as=([@]=at [x]=1)
unset as[@]
declare -p as
ar=(4 5)
BASH_COMPAT=5.1
unset ar[@]
declare -p ar
unset as[*]
declare -p as
BASH_COMPAT=52
ar=(6)
unset ar[*]
declare -p ar
