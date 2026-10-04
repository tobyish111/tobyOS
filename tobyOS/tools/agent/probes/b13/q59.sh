exec 2>&1 </dev/null
cd /tmp
cd -e /tmp; echo "e=$? $PWD"
cd -P /tmp; echo "P=$?"
cd -Pe /; echo "Pe=$? $PWD"
cd -L -P /tmp; echo "LP=$?"
cd -@ /tmp; echo "at=$?"
cd -x; echo "x=$?"
cd -- -e 2>&1 | sed 's/^[^:]*: line [0-9]*: //'
cd - >/dev/null; echo "dash=$?"
cd /tmp
hash -d nope; echo "hd=$?"
/bin/true; hash -d nope; echo "hd2=$?"
printf 'a\nb\nc\nd\ne\n' > /tmp/q59.txt
exec 7< /tmp/q59.txt
mapfile -u 7 arr; echo "u=$? ${#arr[@]} ${arr[4]}"
exec 7<&-
mapfile -u 9 arr; echo "u9=$?"
mapfile -u x arr; echo "ux=$?"
cb() { echo "cb[$1]=$2"; }
mapfile -t -C cb -c 2 arr < /tmp/q59.txt; echo "C=$? ${#arr[@]}"
mapfile -t -C 'echo X' -c 1 arr < /tmp/q59.txt; echo "C1=$?"
mapfile -C cb arr < /tmp/q59.txt; echo "Cdefault=$?"
mapfile -c 0 arr < /tmp/q59.txt; echo "c0=$?"
mapfile -c x arr < /tmp/q59.txt; echo "cx=$?"
mapfile -z arr; echo "z=$?"
mapfile '' < /tmp/q59.txt; echo "empty=$?"
readarray -q x; echo "rq=$?"
f() { false; }
mapfile -t -C f -c 1 arr < /tmp/q59.txt; echo "Cfalse=$?"
printf "it's\n" | mapfile -t -C cb -c 1 arr
declare -n r=HOME
[ -R r ]; echo "R=$?"
[ -R HOME ]; echo "RH=$?"
[[ -R r ]]; echo "RR=$?"
declare -n r2
[ -R r2 ]; echo "R2=$?"
test -R nope; echo "Rn=$?"
rm -f /tmp/q59.txt
echo end
