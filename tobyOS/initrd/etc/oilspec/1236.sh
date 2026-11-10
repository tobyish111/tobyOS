trap 'echo hi' 9
echo status=$?

trap 'echo hi' KILL
echo status=$?

trap 'echo hi' STOP
echo status=$?

trap 'echo hi' TERM
echo status=$?
