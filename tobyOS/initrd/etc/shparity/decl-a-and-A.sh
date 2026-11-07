# -a and -A given together: -A makes the name associative and -a then
# cannot convert it -- "declare: N: cannot convert associative to indexed
# array", status 1 -- after a written list has been made (as -A's) and
# before a quoted one, which is not made. Over an indexed array -A's own
# refusal comes first. tsh held the two options as one, -A's, and said
# nothing (finding 631).
exec 2>&1
declare -aA N=(1 2); echo "new: $?"; declare -p N
declare -a -A M=([k]=v); echo "two options: $?"; declare -p M
declare -aA E; echo "no value: $?"; declare -p E
declare -A A=([a]=x); declare -aA A=(1 2); echo "over -A: $?"; declare -p A
declare -A B=([a]=x); declare -aA B="([b]=y)"; echo "quoted: $?"; declare -p B
s=1; declare -aA s=([k]=v); echo "over a scalar: $?"; declare -p s
f() { local -aA L=([k]=v); echo "local: $?"; declare -p L; }
f
G() { declare -g -aA C=(3 4); echo "-g: $?"; }
declare -A C=([c]=z); G; declare -p C
declare -a I=(x); declare -aA I; echo "over -a: $?"; declare -p I
