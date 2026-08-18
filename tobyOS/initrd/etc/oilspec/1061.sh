ulimit -f

# an arg
ulimit -f -- -42
if test $? -ne 0; then
  echo pass
else
  echo fail
fi
