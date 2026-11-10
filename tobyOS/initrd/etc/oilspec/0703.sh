touch $TMP/setuid $TMP/setgid
chmod u+s $TMP/setuid
chmod g+s $TMP/setgid

test -u $TMP/setuid
echo status=$?

test -u $TMP/setgid
echo status=$?

test -g $TMP/setuid
echo status=$?

test -g $TMP/setgid
echo status=$?
