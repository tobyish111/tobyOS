dirs -c; pushd -n; echo "empty -n rc=$?"; dirs
cd /tmp
pushd -n /usr; echo "rc=$? pwd=$PWD"
pushd -n usr; echo "rc=$?"; dirs -v
pushd -n; echo "rc=$? pwd=$PWD"; dirs
popd -n; echo "rc=$? pwd=$PWD"; dirs
popd -n; echo "rc=$?"; dirs
popd -n; echo "rc=$?"; dirs
popd -n; echo "rc=$?"
pushd -n /nonexist; echo "rc=$?"; dirs
dirs -c
pushd -x /usr; echo "rc=$?"
pushd -n -- /var; echo "rc=$?"
pushd -- /usr >/dev/null; echo "rc=$? $PWD"; dirs
popd -n -- ; echo "rc=$?"
popd x; echo "rc=$?"
popd -n x; echo "rc=$?"
pushd /usr /var; echo "rc=$?"
