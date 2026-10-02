# A variable DECLARED but never given a value (bash: "invisible" until
# assigned): how each kind of read sees it.
exec 2>&1
declare x
declare -p x; echo "1 [${x-unset}] [${x+set}] [${#x}]"
[[ -v x ]]; echo "2 $?"
set | grep '^x=' ; echo "3 $?"
declare -p | grep ' x$\| x=' ; echo "4"
echo "5 [${!x*}] [$(compgen -v x 2>/dev/null)]"
declare -x ex; env | grep '^ex=' ; echo "6 $?"; declare -p ex
declare -r ro; declare -p ro; ro=1; echo "7 $?"
declare -i ii; declare -p ii; ii=1+2; declare -p ii
declare -a aa; declare -p aa; echo "8 [${aa[@]-unset}] [${#aa[@]}] [${aa-u}]"
aa=(); declare -p aa
declare -A AA; declare -p AA; AA[k]=v; declare -p AA
x=; declare -p x
unset x; declare -p x
f() { local l; declare -p l; echo "9 [${l-unset}]"; l=v; declare -p l; }; l=outer; f; declare -p l
g() { local -a la; declare -p la; local lr=; declare -p lr; }; g
h() { declare d=in; declare -p d; }; d=out; h; declare -p d
k() { declare -g gg=global; local; }; k; declare -p gg
set -u
declare u2; echo "10 [${u2-unset}]"; echo "$u2"
echo end
