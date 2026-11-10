# A variable DECLARED but never given a value is bash's "invisible" one:
# unset to every read, yet it exists -- `declare -p` prints it, and its
# attributes (-i -l -u -r -x) wait for the first value. tsh either created
# it empty or not at all, and had no -i, -l or -u.
exec 2>&1
declare x
declare -p x; echo "1 [${x-unset}] [${x+set}] [${#x}]"
[[ -v x ]]; echo "2 $?"
declare -r ro; declare -p ro; ro=1; echo "7 $?"
declare -i ii; declare -p ii; ii=1+2; declare -p ii
declare -l lo=AbC; declare -u up=dEf; declare -p lo up
declare -a aa; declare -p aa; echo "8 [${aa[@]-unset}] [${#aa[@]}]"
aa=(); declare -p aa
declare -A AA; declare -p AA; AA[k]=v; declare -p AA
x=; declare -p x
f() { local l; declare -p l; echo "9 [${l-unset}]"; l=v; declare -p l; }; l=outer; f; declare -p l
h() { declare d=in; declare -p d; }; d=out; h; declare -p d
k() { declare -g gg=global; }; k; declare -p gg
echo end
