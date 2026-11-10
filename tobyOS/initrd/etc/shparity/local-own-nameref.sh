# local on the function's own reference works on what it names -- `local
# -i ref=([1]=)` makes a local -i array var; a reference the function did
# not make is hidden by a new local (finding 329).
{
f() { local -n ref=var; local -i ref=([1]=); declare -p ref var; }
unset var; f; var=0; f; declare -p var
g() { local -n ref=var; local ref=1; declare -p ref var; }; var=0; g; declare -p var
T=1; declare -n R=T; h() { local -i R; R=2+2; declare -p R T; }; h; declare -p R T
} 2>&1
