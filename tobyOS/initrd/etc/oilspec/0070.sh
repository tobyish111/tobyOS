$SH -c '
set -- a b c
shift 1 extra
'
if test $? -eq 0; then
  echo fail
fi
