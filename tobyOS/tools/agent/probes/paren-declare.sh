x='(1 2)'
declare -a a=$x; declare -p a
declare b=$x; declare -p b
declare -a c="$x"; declare -p c
declare -A e='([k]=v)'; declare -p e
declare f='(3 4)'; declare -p f
declare -a g='(5 6)'; declare -p g
f2() { local -a d=$x; declare -p d; local h=$x; declare -p h; }; f2
export k='(7 8)'; declare -p k
readonly r='(9 0)'; declare -p r
declare -a m=(1 2); m+=$x; declare -p m
n=(1); n=$x; declare -p n
typeset -a t=\(a\ b\); declare -p t
declare -a u=('(p q)'); declare -p u
