$SH -e -c '
trap "echo noprint" EXIT
trap 0 EXIT
echo ok0
'
echo

$SH -e -c '
trap "echo noprint" EXIT
trap " 42 " EXIT
echo ok42space
'
echo

# corner case: sometimes 07 is treated as octal, but not here
$SH -e -c '
trap "echo noprint" EXIT
trap 07 EXIT
echo ok07
'
echo

$SH -e -c '
trap "echo trap-exit" EXIT
trap -1 EXIT
echo bad
'
if test $? -ne 0; then
  echo failure
fi
