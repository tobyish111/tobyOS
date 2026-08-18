trap 'foo' SIGINVALID
if test $? -ne 0; then
  echo ok
fi

trap - SIGINVALID
if test $? -ne 0; then
  echo ok
fi
