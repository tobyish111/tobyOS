rm -f no-clobber
set -C

echo foo > no-clobber
echo create=$?

echo overwrite > no-clobber
echo overwrite=$?

echo force >| no-clobber
echo force=$?

cat no-clobber
