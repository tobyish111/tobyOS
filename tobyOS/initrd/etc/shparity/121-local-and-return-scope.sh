local a=1; echo "L01 top st=$? a=${a-unset}"
f2() { local b=2; echo "L02 fn st=$? b=$b"; }; f2; echo "L02 after b=${b-unset}"
src=./lsrc.121
echo 'local c=3; echo "L03 sourced-top st=$? c=${c-unset}"' > "$src"
. "$src"
f4() { . "$src"; }; f4
f5() { ( local d=4; echo "L05 subshell-in-fn st=$? d=$d" ); }; f5
eval 'local e=5'; echo "L06 eval-top st=$?"
f7() { eval 'local g=7'; echo "L07 eval-fn st=$? g=$g"; }; f7
declare h=8; echo "L08 declare-top st=$? h=$h"
f9() { local; echo "L09 bare-local st=$?"; }; f9
local; echo "L10 bare-local-top st=$?"
rm -f "$src"
echo R before
return 3
echo "R after st=$?"
rsrc=./rs.121
echo "echo S in; return 4; echo S no" > "$rsrc"
. "$rsrc"; echo "R2 sourced-return st=$?"
f() { return 5; }; f; echo "R3 fn st=$?"
eval "return 6"; echo "R4 eval-top st=$?"
rm -f "$rsrc"
echo done
