# note: it's lame that the 'false' case is confused with the 'typo' case.
# but checking for error code 2 is unlikely anyway.
test -o nounset
echo status=$?

set -o nounset
test -o nounset
echo status=$?

test -o _bad_name_
echo status=$?
