# Hm I don't understand why the array only has one element.  I guess because
# index 0 is used twice?
declare -a 'array=([a]=b [c]=d)'
declare -A 'assoc=([a]=b [c]=d)'
argv.py "${#array[@]}" "${!array[@]}" "${array[@]}"
argv.py "${#assoc[@]}" "${!assoc[@]}" "${assoc[@]}"
