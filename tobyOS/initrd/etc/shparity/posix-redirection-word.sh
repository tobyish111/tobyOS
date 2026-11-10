# In posix mode a non-interactive shell neither splits nor globs a
# redirection's word: `cat < a.x*` opens a.x*, f=' a ' writes " a ", "$@"
# joins with a blank -- only an expansion to nothing is still ambiguous
# (finding 408).
exec 2>&1
d=$(mktemp -d); cd "$d" || exit
echo hi > a.x1
set -o posix
cat < a.x*; echo "rc=$?"
v=$(< a.x*); echo "[$v] rc=$?"
f=' a '; echo hu > $f; set -- b c; echo ho > "$@"; f=; echo no > $f; echo "rc=$?"
HOME=$d; echo ti > ~/t1
set +o posix
for n in *; do echo "<$n>"; done
cd /; rm -rf "$d"
