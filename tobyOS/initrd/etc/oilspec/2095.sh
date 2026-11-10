echo hi > a-{one,two}
echo status=$?

head a-*
echo status=$?
