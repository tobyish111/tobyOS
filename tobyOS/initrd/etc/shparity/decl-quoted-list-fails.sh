# A quoted list a declaration builtin takes as one -- `declare -a
# U="( ... )"` -- is the builtin's to make, not the expansion's: when it
# fails at its first element U is the empty array the builtin's -a made,
# with readonly's -r and export's -x on it, and the builtin stops there,
# the rest of the line with it. Over a name only declared -a, U stays so.
# tsh left U only declared (614's rule for a written list), went on to
# the words after it, and listed export -a's empty array as only declared
# (bash's array19.sub; finding 624).
exec 2>&1
value='[$(echo total 0)]=1 [2]=2]'
declare -a var="($value)"
declare -p var
declare -a U="( [1+]=x )"
declare -p U
readonly -a R="( [1+]=x )"
declare -p R
export -a X="( [1+]=x )"
declare -p X
export -a E="()"
declare -p E
declare -a D; declare D="( [1+]=x )"
declare -p D
declare -a W="( [1+]=x )" V=1; echo "not reached"
declare -p W V
v='[0]=a [1+]=b'; declare -a S="($v)"
declare -p S
