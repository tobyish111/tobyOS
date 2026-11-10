umask 0002  # begin in a known state for the test
# open()s 'umask-one' with mask 0666, then subtracts 0002 -> 0664
echo one > $TMP/umask-one

umask g-w,o-w
echo two > $TMP/umask-two

stat -c '%a' $TMP/umask-one $TMP/umask-two
