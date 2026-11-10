x=42
readonly x
export x

declare -p x undef1 undef2 2> de

typeset -p x undef1 undef2 2> ty

# readonly -p and export -p don't accept args!  They only print all
#
# These do not accept args
# readonly -p x undef1 undef2 2> re
# export -p x undef1 undef2 2> ex

f() {
  # it behaves weird with x
  #local -p undef1 undef2 2>lo
  local -p a b b>lo
  #local -p x undef1 undef2 2> lo
}
# local behaves differently in bash 4.4 and bash 5, not specifying now
# f
# files='de ty lo'

files='de ty'

wc -l $files
#cat $files
