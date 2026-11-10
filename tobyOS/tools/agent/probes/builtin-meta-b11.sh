command -v echo ZZZ; echo st=$?
command -v ZZZ echo; echo st=$?
command -v ZZZ; echo st=$?
command -V nonexistent; echo st=$?
command -V echo ZZZ; echo st=$?
builtin; echo st=$?
builtin --; echo st=$?
builtin -- false; echo st=$?
builtin -x; echo st=$?
exit 7 8 9
echo st=$?
exit abc
echo notreached
