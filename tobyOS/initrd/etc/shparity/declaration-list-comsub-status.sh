# A DECLARATION'S STATUS IS ITS OWN, NOT THE LAST $( ) OF ITS LIST. The
# words of `declare -a a=( ... )` are declare's to expand, and a failing
# substitution among them sets $? for the words after it -- but declare
# itself succeeds, so the status after it is 0, and set -e goes on. A
# plain assignment's status is still its last substitution's.
declare -a a=( one "$(exit 3)" two ); echo "declare $? ${#a[@]}"
f() {
    local -a l=( "$(exit 4)" ); echo "local $?"
    local m=( "$(exit 5)" "$?" ); echo "local2 $? [${m[*]}]"
}
f
readonly -a r=( "$(exit 6)" ); echo "readonly $?"
typeset -A h=( [k]="$(exit 7)" ); echo "typeset $?"
declare -a s=( "$(exit 8)" "$?" ); echo "inside [${s[*]}]"
b=( "$(exit 9)" ); echo "plain $?"
eval 'declare -a v=( "$(exit 2)" )'; echo "eval $?"
set -e
declare -a e=( "$(false)" ); echo "errexit goes on"
b=( "$(false)" ); echo "not reached"
