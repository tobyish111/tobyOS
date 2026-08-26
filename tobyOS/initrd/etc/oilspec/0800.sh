pushd -z
echo status=$?
pushd /tmp >/dev/null
echo status=$?
pushd -- /tmp >/dev/null
echo status=$?
