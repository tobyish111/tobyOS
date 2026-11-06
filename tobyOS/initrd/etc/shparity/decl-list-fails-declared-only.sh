# A new name whose list fails at its first element is only declared --
# `declare U=( [1+]=x )` leaves `declare -a U`, no value. An element given
# before the failure stays, an array that was there is emptied, and
# export's -x and readonly's -r do not go on. tsh left an empty array,
# U=() (finding 614).
exec 2>&1
declare U=( [1+]=x )
declare -p U
declare -i V=( [1/0]=y )
declare -p V
declare W=( [0]=a [1+]=b )
declare -p W
export X=( [1+]=x )
declare -p X
readonly Y=( [2-]=y )
declare -p Y
P=(p q)
declare P=( [1+]=x )
declare -p P
