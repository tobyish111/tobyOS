shopt -s strict_array

typeset -a a
a=(1 2 3)

export a
printenv.py a
