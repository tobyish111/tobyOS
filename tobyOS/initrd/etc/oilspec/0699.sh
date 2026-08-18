test -s __nonexistent
echo status=$?
touch $TMP/empty
test -s $TMP/empty
echo status=$?
echo nonempty > $TMP/nonempty
test -s $TMP/nonempty
echo status=$?
