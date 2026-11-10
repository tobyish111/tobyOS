trap 'foo'
if test $? -ne 0; then
  echo ok
fi
