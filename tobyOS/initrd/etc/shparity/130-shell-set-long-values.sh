s=abcdefghij; for i in 1 2 3 4 5; do s="$s$s"; done
t=${s:0:100}
echo "r0 [${#s}]"
read -r r <<< "$s"; echo "r1 [$?] [${#r}]"
printf '%s\n' "$s" | { read -r r; echo "r2 [${#r}]"; }
for it in "$s"; do echo "r3 [${#it}]"; done
unset u; : "${u:=$s}"; echo "r4 [${#u}]"
printf -v pv '%s' "$s"; echo "r5 [${#pv}]"
read -r -a arr <<< "$t $s"; echo "r6 [${#arr[@]}] [${#arr[1]}]"
IFS=: read -r a b <<< "$s:$t"; echo "r7 [${#a}] [${#b}]"
n=0; while read -r line; do n=$((n + ${#line})); done <<< "$s"; echo "r8 [$n]"
f() { local lv; read -r lv <<< "$s"; echo "r9 [${#lv}]"; }; f
echo done
