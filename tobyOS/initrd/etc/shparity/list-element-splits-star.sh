# A list's element is a word that splits, as an argument is: under a null
# IFS `l=( $* )` is a field per parameter. tsh expanded the elements as an
# assignment's value and ran them together (finding 561).
exec 2>&1
set -- a 'b c' d; arr=(a 'b c' d)
IFS=
l=( $* ); echo "${#l[@]} [${l[1]}]"
l=( ${A-$*} ); echo "${#l[@]}"
l=( ${arr[*]} ${arr[*]/x/y} ); echo "${#l[@]}"
l=( $(:) $* ); echo "${#l[@]}"
declare -A m=([k]=a [j]=b); l=( ${m[*]} ); echo "${#l[@]}"
