for arg in zz unlimited; do
  echo "  arg $arg"
  ulimit -f
  echo status=$?
  ulimit -f $arg
  if test $? -ne 0; then
    echo 'FAILED'
  fi
  echo
done
