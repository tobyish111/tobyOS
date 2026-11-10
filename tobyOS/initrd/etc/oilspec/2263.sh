# variant of above case

$SH -c --
if test $? -ne 0; then
  echo failed
fi

$SH -c -
if test $? -ne 0; then
  echo failed
fi
