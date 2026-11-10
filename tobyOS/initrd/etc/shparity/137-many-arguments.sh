lst=; i=0; while [ $i -lt 5000 ]; do i=$((i+1)); lst="$lst $i"; done
set -- $lst; echo "g1 [$#] [$1] [${2500}] [${5000}]"
f() { echo "g2 [$#] [${4999}]"; }; f $lst
shift 4000; echo "g3 [$#] [$1]"
n=0; for w in $lst; do n=$((n+1)); done; echo "g4 [$n]"
a=($lst); echo "g5 [${#a[@]}]"
printf '%s\n' $lst | { c=0; while read -r l; do c=$((c+1)); done; echo "g6 [$c]"; }
x=$(printf '%s ' "$@"); echo "g7 [${#x}]"
set -- "$@" "$@"; echo "g8 [$#]"
echo done
