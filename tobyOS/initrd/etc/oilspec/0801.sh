pushd / >/dev/null
popd zzz
echo status=$?

popd -- >/dev/null
echo status=$?

popd -z
echo status=$?
