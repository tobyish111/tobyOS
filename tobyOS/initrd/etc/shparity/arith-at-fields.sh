# In $(( )) the fields of $@ join with a space, as in any string: `set --
# '1+2 '; echo $(( $@ ))` is 3 (finding 396). tsh kept the field mark in
# the expression, "operand expected".
exec 2>&1
set -- '1+2 '; echo $(( $@ )); echo "$(( $@ ))"
set -- 2 '*' 3; echo $(( $@ )); echo $(( $* ))
