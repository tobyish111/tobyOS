# A ${v:=w} that cannot assign is an expansion error: its one message, the
# rest of the line dropped, status 2 -- 1 when set -e ends the shell
# (finding 333). tsh added "failed to assign" and ran on with status 0.
{ readonly x; : ${x:=1}; echo "same line"; } 2>&1
echo "next $?"
declare -n r
{ : ${r:=/}; echo "same line"; } 2>&1
echo "next $?"
f() { readonly y; : ${y:=1}; echo "in f"; }; { f; echo "after f $?"; } 2>&1
echo "next $?"
( readonly z; : ${z:=1}; echo inner ) 2>&1; echo "sub $?"
( set -e; readonly w; : ${w:=1}; echo "not here" ) 2>&1; echo "e $?"
