shopt -s failglob

echo hi > zz-*-xx
echo status=$?

echo zz*
echo status=$?
