$SH -c 'exit 255'
echo status=$?

$SH -c 'exit 256'
echo status=$?

$SH -c 'exit 257'
echo status=$?

echo ===

$SH -c 'exit -1'
echo status=$?

$SH -c 'exit -2'
echo status=$?
