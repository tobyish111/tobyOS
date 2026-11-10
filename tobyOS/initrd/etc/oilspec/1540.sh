$SH -c 'set -e; false || { echo group; false; }; echo bad'
echo status=$?
echo

$SH -c 'set -e; false || ( echo subshell; exit 42 ); echo bad'
echo status=$?
echo

# noforklast optimization
$SH -c 'set -e; false || /bin/false; echo bad'
echo status=$?
