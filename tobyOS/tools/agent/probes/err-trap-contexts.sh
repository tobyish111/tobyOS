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
