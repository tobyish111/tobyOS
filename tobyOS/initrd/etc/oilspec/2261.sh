$SH -c -z 'echo z'
if test $? -ne 0; then
  echo 'z failed'
fi
echo

$SH -c --- 'echo three'
if test $? -ne 0; then
  echo three failed
fi
echo

$SH -c -- 'echo two'
echo two=$?
echo

$SH -c - 'echo one'
echo one=$?
echo

$SH -c '' 'echo zero'
echo zero=$?
echo

# odd
$SH -c 'echo aa' 'echo bb'
echo aa=$?
