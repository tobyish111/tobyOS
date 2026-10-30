# [ -v NAME ] of an array asks about its element 0, whatever made the array:
# one with elements but no element 0 is unset to -v, as to ${NAME-x}
# (bash's builtins5.sub). 371 had tsh's -v take the array's own entry for
# a set scalar once an element had been assigned.
exec 2>&1
typeset -A A; A[a]=1; [ -v A ]; echo "$? [${A-u}] [${A+s}]"; [[ -v A ]]; echo $?
typeset -a a; a[1]=1; [ -v a ]; echo "$? [${a-u}] [${a+s}]"; [[ -v a ]]; echo $?
typeset -A B=([a]=1); [ -v B ]; echo $?
b=(x); [ -v b ]; echo "$? [${b-u}]"
