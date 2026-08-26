# non-interactive
$SH -c 'set -u; echo before; echo $x; echo after'
if test $? -ne 0; then
  echo OK
fi

# interactive
$SH -i -c 'set -u; echo before; echo $x; echo after'
if test $? -ne 0; then
  echo OK
fi
