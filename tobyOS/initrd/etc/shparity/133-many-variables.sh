i=0; while [ $i -lt 2000 ]; do eval "var_$i=v$i"; i=$((i+1)); done
echo "e1 [$var_0] [$var_511] [$var_512] [$var_1999]"
unset var_700; var_new=n; echo "e2 [$var_new] [${var_700-unset}]"
n=$(set | grep -c "^var_"); echo "e3 [$n]"
export var_1500; x=$(sh -c 'echo "$var_1500"'); echo "e4 [$x]"
echo "e5 [${!var_19*}]" | wc -w | sed 's/^/e5 /'
i=0; while [ $i -lt 40 ]; do readonly ro_$i=r; i=$((i+1)); done
(ro_35=x) 2>/dev/null; echo "e6 [$?] [$ro_35]"
A=a; A+=b sh -c 'echo "e7 [$A]"'; echo "e8 [$A]"
echo done
