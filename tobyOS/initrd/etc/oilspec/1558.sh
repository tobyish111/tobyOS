$SH -c '
set -o errexit
true > /
echo builtin status=$?
'
echo status=$?

$SH -c '
set -o errexit
/bin/true > /
echo extern status=$?
'
echo status=$?

$SH -c '
set -o errexit
assign=foo > /
echo assign status=$?
'
echo status=$?
