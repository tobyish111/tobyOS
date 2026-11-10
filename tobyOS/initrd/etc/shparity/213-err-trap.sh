# The ERR trap: a failing ( ), [[ ]] and (( )) fire it as a command does;
# under set -E (errtrace) it reaches into functions, subshells and command
# substitutions; with set -e it runs before the shell exits. tsh fired it
# for neither compound and refused -E; $- now carries B, and E under -E.
exec 2>&1
trap 'echo "err $LINENO st=$?"' ERR
{ false; }
echo -
( false )
echo -
( exit 3 )
echo -
if false; then :; fi
echo -
while false; do :; done
echo -
[[ a == b ]]
(( 0 ))
x=$(false)
echo -
f() { false; return 2; }
f
echo -
case x in y) ;; esac
for i in; do :; done
{ true; false; } || true
echo -
! false
false | true
true | false
echo end
set -E
g() { false; echo in-g; }
g
( false; echo in-sub )
y=$(false; echo in-cs)
echo "$y"
echo "$-"
set -E; echo "$-"; set +E
trap 'echo "after errexit $?"' ERR; set -e; ( exit 99 ); echo no
