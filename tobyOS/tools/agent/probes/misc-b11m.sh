stdout_err() { echo STDOUT; echo STDERR >&2; }
stdout_err |& sort
{ echo g1; echo g2 >&2; } |& sort
stdout_err |& cat > _p.txt; sort _p.txt; rm -f _p.txt
echo x | cat |& cat
a=(x y z); (echo "[${!a[@]-d}]"); echo st=$?
a=(x); x=val; echo "[${!a[@]-d}]" "[${!a[@]:1}]"
declare -A A=([B]=b); echo "[${!A}]"; echo st=$?
e=(); echo "[${!e}]"
unset u; (echo "[${!u}]"); echo st=$?
hooks='pre[@]'; pre=('foo bar' baz); printf '<%s>' "${!hooks}"; echo; printf '<%s>' ${!hooks+"${!hooks}"}; echo
f() { local v=" -L"; eval "X_foo"+='$v'; export X_foo; }; f; declare -p X_foo
a=(4 5 6); (( b = a )); echo "[$b]"; (( c = a + 1 )); echo "[$c]"
