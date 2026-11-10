shopt -s strict_array

typeset -A a
a["foo"]=bar

export a
printenv.py a
