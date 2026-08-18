$SH -c '
set -u
test_function_2() {
  x=$blarg
}
test_function() {
  eval "test_function_2"
}

echo before
eval test_function
echo after
'
# status must be non-zero: bash uses 1, ash/dash exit 2
if test $? -ne 0; then
  echo OK
fi
