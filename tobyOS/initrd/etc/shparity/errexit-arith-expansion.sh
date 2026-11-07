# Under set -e an arithmetic expansion's error -- `$((1+))`, `$[1/0]` --
# only throws the rest of its line away: the script goes on at the next
# line with status 1, as it does without set -e. A bad substitution under
# set -e does end the shell. tsh ended it for both (finding 620).
exec 2>&1
set -e
echo $((1+)); echo "not this"
echo "next line: $?"
x=$((2/0)); echo "not this either"
echo "after an assignment: $?"
for i in 1 2; do echo "body $i"; y=$[3+]; done; echo "not after the loop"
echo "after the loop: $?"
f() { z=$((4+)); echo "not in f"; }; f; echo "not after f"
echo "after f: $?"
[[ $((5+)) == 1 ]]; echo "not after [[ ]]"
echo "after [[ ]]: $?"
( echo ${a[}; echo "the subshell did not get here" ); echo "not after the subshell"
echo "not reached"
