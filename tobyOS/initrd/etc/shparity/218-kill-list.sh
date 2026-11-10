# kill -L is -l; -l 0 is EXIT; a status above 128 names its signal; a bad
# operand is "invalid signal specification" (bash). tsh refused -L and 0.
exec 2>&1
kill -L 2; kill -l 0; kill -l 130; kill -l TERM; kill -l SIGHUP
kill -l ZZZ; echo st=$?
kill -l 999; echo st=$?
