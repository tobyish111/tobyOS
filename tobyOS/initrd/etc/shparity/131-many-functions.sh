i=0; while [ $i -lt 100 ]; do eval "fn_$i() { echo f$i; }"; i=$((i+1)); done
echo "m1 [$(fn_0)] [$(fn_31)] [$(fn_32)] [$(fn_99)]"
unset -f fn_50; fn_new() { echo new; }; echo "m2 [$(fn_new)] [$(fn_99)]"
n=0; for f in $(declare -F | cut -d' ' -f3); do n=$((n+1)); done; echo "m3 [$n]"
g() { g() { echo second; }; echo first; }; g; g
echo done
