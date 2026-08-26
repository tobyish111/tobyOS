umask 0002
echo one > $TMP/umask-one

umask 0022
echo two > $TMP/umask-two

stat -c '%a' $TMP/umask-one $TMP/umask-two
