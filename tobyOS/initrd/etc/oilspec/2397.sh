# dynamic parsing of first argument.
declare +a 'xyz1=1'
declare +a 'xyz2=(2 3)'
declare -a 'xyz3=4'
declare -a 'xyz4=(5 6)'
argv.py "${xyz1}" "${xyz2}" "${xyz3[@]}" "${xyz4[@]}"
