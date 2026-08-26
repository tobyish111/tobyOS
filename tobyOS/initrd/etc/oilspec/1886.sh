case $SH in dash) exit ;; esac


test -f $'\0'
echo status=$?

touch foo
test -f $'foo\0'
echo status=$?

test -f $'foo\0bar'
echo status=$?

test -f $'foobar'
echo status=$?
