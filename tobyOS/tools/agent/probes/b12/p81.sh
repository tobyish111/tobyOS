exec 2>&1
coproc P ( echo sub-$((2+3)) )
read -r a <&"${P[0]}"; echo "paren: $a"
wait $P_PID; echo "pw=$?"
coproc W while read -r l; do echo "w:$l"; done
echo one >&"${W[1]}"; read -r b <&"${W[0]}"; echo "$b"
exec {W[1]}>&-
wait $W_PID; echo "ww=$?"
coproc { echo grp; exit 3; }
cp=$COPROC_PID; read -r c <&"${COPROC[0]}"; echo "grp: $c"
wait $cp; echo "gw=$?"
coproc A (( 1 + 1 ))
wait $A_PID; echo "aw=$?"
coproc C [[ x == x ]]
wait $C_PID; echo "cw=$?"
coproc cat /dev/null
wait $COPROC_PID; echo "cat=$?"
f() { coproc LF { echo inf; }; read -r z <&"${LF[0]}"; echo "f: $z"; wait $LF_PID; }
f; echo "fw=$?"
coproc E { echo err >&2; } 2>&1
read -r e <&"${E[0]}"; echo "e: $e"; wait $E_PID
coproc echo direct; read -r d <&"${COPROC[0]}"; echo "d: $d"; wait
coproc Z (( 0 )); zp=$Z_PID; wait $zp; echo "zw=$?"
coproc { kill -TERM $BASHPID; }; kp=$COPROC_PID; wait $kp; echo "kw=$?"
type coproc; compgen -k | tail -2
echo done
