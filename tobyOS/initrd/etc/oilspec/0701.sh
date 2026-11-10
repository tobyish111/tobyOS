mkfifo $TMP/fifo
test -p $TMP/fifo
echo status=$?

test -p testdata
echo status=$?
