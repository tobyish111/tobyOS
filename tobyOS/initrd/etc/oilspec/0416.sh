# This means that for ASSIGNMENT_WORD, on the RHS you invoke the parser again!
# Could be any kind of quoted string.
FOO="foo" BAR="[$FOO][$BAZ]" BAZ=baz printenv.py FOO BAR BAZ
