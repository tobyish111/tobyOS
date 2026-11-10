# Indirection: ${!a[@]OP} with an operator is an indirection through a's
# values joined by blanks (not the keys); a reference that exists without
# a value expands to nothing; only a name that is no variable at all is
# "invalid indirect expansion". tsh listed keys and errored on both.
exec 2>&1
a=(x y z); (echo "[${!a[@]-d}]"); echo st=$?
a=(x); x=val; echo "[${!a[@]-d}]" "[${!a[@]:1}]"
declare -A A=([B]=b); echo "[${!A}]"; echo st=$?
e=(); echo "[${!e}]"
unset u; (echo "[${!u}]"); echo st=$?
hooks='pre[@]'; pre=('foo bar' baz); printf '<%s>' "${!hooks}"; echo
a=(4 5 6); (( b = a )); echo "[$b]"; (( c = a + 1 )); echo "[$c]"
