case $SH in bash) exit ;; esac

ulimit -a 42
if test $? -ne 0; then
  echo 'failure that was expected'
fi
