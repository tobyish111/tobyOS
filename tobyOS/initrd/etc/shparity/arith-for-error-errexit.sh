# An arithmetic error in a `for (( ))` header -- its start, its test or
# its step -- makes the loop fail with status 1, a failure set -e does not
# act on and the ERR trap does not see: the rest of the line runs. tsh's
# set -e ended the script there (finding 621).
exec 2>&1
set -e
trap 'echo "ERR trap ran"' ERR
for (( i=1+; i<2; i++ )); do echo body; done; echo "start: $?"
for (( i=0; i<1+; i++ )); do echo body; done; echo "test: $?"
for (( i=0; i<2; i=i+ )); do echo "body $i"; done; echo "step: $?"
f() { for (( j=2+; ; )); do :; done; echo "in f: $?"; }; f
eval 'for (( k=3+; ; )); do :; done; echo "in eval: $?"'
echo "still running"
false
echo "not reached"
