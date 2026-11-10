# Element operations: negative-index appends, long names, subscripts
# expanded after the value, scalar-as-array slices, "$@" and arrays inside
# ${u:-...} words, assigning a scalar to an array.
# (batch 12; probe p37 in C:\t\b12)
exec 2>&1
a=(1 2); a=5; declare -p a
b=(1 2); b+=x; declare -p b
declare -a c; c=7; declare -p c
declare -a 'd=4'; declare -p d
declare -a e=8; declare -p e
x='f=(1 2)'; declare "$x"; declare -p f
declare -a "$x"; declare -p f
declare +a 'g=(2 3)'; declare -p g
declare 'h=(2 3)'; declare -p h
i=(9); declare 'i=(2 3)'; declare -p i
declare -A A; A=v; declare -p A
export 'j=(1 2)'; declare -p j
local_f() { local 'k=(1 2)'; declare -p k; local -a 'l=(1 2)'; declare -p l; }; local_f
readonly 'm=(1 2)'; declare -p m
