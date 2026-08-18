# I think this will test write() errors, rather than the final flush() error
# (which is currently skipped by C++

{ echo 'ulimit -f 1'
  # More than 8 KiB may cause a flush()
  python2 -c 'print("echo " + "X"*9000 + " >out.txt")'
  echo 'echo inner=$?'
} > big.sh

$SH big.sh
echo outer=$?


# not sure why this is different
