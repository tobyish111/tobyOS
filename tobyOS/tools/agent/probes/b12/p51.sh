exec 2>&1
a=1; b=2
readonly -p a; echo "1 $?"; declare -p a
export -p b; echo "2 $?"; declare -p b
f() { local c=3; local -p c a; echo "3 $?"; local -p; echo "4 $?"; }; f
readonly -p | grep -c 'declare -r a='
export -p | grep -c 'declare -x b='
readonly -p nosuch; echo "5 $?"; declare -p nosuch
