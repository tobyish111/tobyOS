sh=$(which $SH)

unset SHELL

prog='
if test -n "$SHELL"; then
  # the exact value is different on CI, so do not assert
  echo SHELL is set
  echo SHELL=$SHELL >&2
fi
'

$SH -c "$prog"

$SH -i -c "$prog"

# make it a login shell
$SH -l -c "$prog"
