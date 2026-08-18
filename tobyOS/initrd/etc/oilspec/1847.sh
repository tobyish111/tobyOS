typeset  -n ref

# This is technically incorrect: an undefined name shouldn't evaluate to empty
# string.  mksh doesn't allow it.
echo ref=$ref

echo nounset
set -o nounset
echo ref=$ref
