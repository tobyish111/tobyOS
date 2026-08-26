set -o errexit

echo foo > can-clobber
echo status=$?
set +C

echo foo > can-clobber
echo status=$?
set +o noclobber

echo foo > can-clobber
echo status=$?
cat can-clobber
