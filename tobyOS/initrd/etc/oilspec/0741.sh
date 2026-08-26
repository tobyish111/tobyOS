mkdir -p /tmp/spam/foo /tmp/eggs/foo

CDPATH='/tmp/spam:/tmp/eggs'

cd foo
echo status=$?
pwd


# doesn't print the dir
