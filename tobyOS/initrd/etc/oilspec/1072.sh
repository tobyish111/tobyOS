# Not sure why these don't work
case $SH in dash|mksh) exit ;; esac


flag=-t

ulimit -S -H $flag 100
echo both=$?

ulimit -S $flag 90
echo soft=$?

ulimit -S $flag 95
echo soft=$?

ulimit -S $flag 105
if test $? -ne 0; then
  echo soft OK
else
  echo soft fail
fi

ulimit -H $flag 200
if test $? -ne 0; then
  echo hard OK
else
  echo hard fail
fi
