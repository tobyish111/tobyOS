# local NAME=VALUE over a reference the function did not make is a plain
# local; the reference outside stays (finding 344). tsh wrote the value
# through the reference.
{
( declare -n ref=var; f() { local ref=Y; declare -p ref var; }; var=X; f; unset -v var; f; declare -p ref )
( declare -n r=t; t=1; g() { local -x r=2; declare -p r t; }; g; declare -p r t )
} 2>&1
