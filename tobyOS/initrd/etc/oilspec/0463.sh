case $SH in dash|mksh) exit ;; esac

echo x &

# here, you can't tell if it's -n or the other
wait -n $!
echo status=$?

# by the bash error, you can tell which is preferred
wait -n $! bad 2>err.txt
echo status=$?
echo

n=$(wc -l < err.txt)
if test "$n" -gt 0; then
  echo 'got error lines'
fi
