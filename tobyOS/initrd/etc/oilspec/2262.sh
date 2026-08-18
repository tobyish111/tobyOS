# variant of above case

$SH -c -- -- 'echo two'
echo status=$?

$SH -c -- -- -- 'echo two'
echo status=$?

$SH -c -z 'echo z'
if test $? -ne 0; then
  echo 'z failed'
fi
