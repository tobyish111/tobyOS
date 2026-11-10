# readonly -a with no names lists the readonly indexed arrays only, -A the
# associative ones, sorted by name; in posix mode as `readonly -a a=(...)`
# and `readonly x="1"` (finding 389). tsh listed every readonly name, in the
# order they were marked.
exec 2>&1
z=(1); b=(x y); declare -r c; readonly b z s=1; declare -A m=([k]=v); readonly m
readonly -a | grep -v BASH_VERSINFO; echo --; readonly -A; echo --; declare -ar | grep -v BASH_VERSINFO
set -o posix
readonly -a | grep -v BASH_VERSINFO; readonly | grep -e ' [bcmsz]'
set +o posix
