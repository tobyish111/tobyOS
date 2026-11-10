mkdir -p $TMP/bin

test -O $TMP/bin
echo status=$?
test -O __nonexistent__
echo status=$?

test -G $TMP/bin
echo status=$?
test -G __nonexistent__
echo status=$?
