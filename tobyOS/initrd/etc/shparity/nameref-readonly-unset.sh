# A readonly reference is unset through -- `declare -rn foo0=bar; unset
# foo0` unsets bar -- and only `unset -n foo0` is refused (finding 345).
# tsh refused both.
{
( bar=one; declare -rn foo0=bar; unset foo0; echo "rc=$?"; declare -p bar; unset -n foo0; echo "rc=$?"; declare -p foo0 )
( bar3=three; declare -rn foo3=bar3; unset foo3; echo "rc=$?"; declare -p bar3 )
} 2>&1
