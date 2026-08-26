# most shells don't verify this
umask 1 2
if test $? -ne 0; then
  echo fail
fi
