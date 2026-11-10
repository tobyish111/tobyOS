# A prefix assignment through a reference binds what it names for the
# command -- in a builtin's, a function's and a child's environment -- and
# the reference stays as it was; one with no target binds the plain name,
# unchecked (finding 338). tsh bound and then removed the reference, and the
# child saw ref=7, not var=7.
{
( unset var; declare -n ref=var; ref=xxx declare -p ref var; echo "after"; declare -p ref var )
( unset var; declare -n ref=var; var= ref=5 declare -p ref var; ref=7 printenv var; echo "[${var-unset}]" )
( var=foo; declare -n ref=var; ref+=bar printenv var; echo "[$var]"; ref=x declare -p var; declare -p var )
( f() { echo "[$r]"; declare -p r; }; declare -n r; r=/ f; declare -p r; r=q declare -p r; declare -p r )
( declare -nx xr=var; var=foo; printenv xr; xr+= printenv xr; printenv var; echo "rc=$?" )
} 2>&1
