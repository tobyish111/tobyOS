# A declaration builtin's list written after a word such as -A, which the
# builtin itself takes for a name, is associative all the same: bash's
# parser marks the list from the words before it. The -A is still "not a
# valid identifier"; a raw ^A in the list is two; with -a given too the
# list is made and then -a says it cannot convert it; over an indexed
# array the list is refused before the builtin runs and the line ends.
# Not $o, not '-A'. tsh made the list indexed (finding 612).
exec 2>&1
declare v -A U=( [k]=x [j]=y ); echo "rc=$?"; declare -p U
f() { local w -Ar L=( [a]=1 ); declare -p L; }; f
readonly r -A R=( [p]=q ); declare -p R
export e -xA E=( [m]=n ); declare -p E
eval "declare v2=$(printf '\001') -A C=( [$(printf '\001')]=x )"; declare -p C
declare -a v3 -A D=( [k]=x ); echo "rc=$?"; declare -p D
o=-A; declare v4 $o F=( [k]=x ); declare -p F
declare v5 '-A' G=( [k]=x ); declare -p G
I=(a b)
declare v6 -A I=( [k]=x ); echo "not reached"
echo "rc=$?"; declare -p I
