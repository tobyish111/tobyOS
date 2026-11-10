# coproc [NAME] command: NAME[0]/NAME[1] and NAME_PID, the default name
# COPROC for a simple command, a compound body with a name, `{NAME[1]}>&-`
# and `exec N>&-` really closing the input so the coprocess sees EOF, the
# variables going when it is reaped. tsh ran a program called coproc.
# Every coprocess whose line uses it after it starts reads a line from
# the shell first: bash disposes of a coprocess that has ended before it
# finishes starting it (its own stop_pipeline reaps it), so a `read` from
# one that exits at once is a race in bash itself.
# (batch 12; probe p81 in C:\t\b12)
exec 2>&1
coproc P ( read -r _; echo sub-$((2+3)) ); echo x >&"${P[1]}"; read -r a <&"${P[0]}"; echo "paren: $a"
wait $P_PID; echo "pw=$?"
coproc W while read -r l; do echo "w:$l"; done
echo one >&"${W[1]}"; read -r b <&"${W[0]}"; echo "$b"
exec {W[1]}>&-
wait $W_PID; echo "ww=$?"
coproc { read -r _; echo grp; exit 3; }
cp=$COPROC_PID; echo go >&"${COPROC[1]}"; read -r c <&"${COPROC[0]}"; echo "grp: $c"
wait $cp; echo "gw=$?"
coproc A (( 1 + 1 ))
wait $A_PID; echo "aw=$?"
coproc C [[ x == x ]]
wait $C_PID; echo "cw=$?"
coproc cat /dev/null
wait $COPROC_PID; echo "cat=$?"
f() { coproc LF { read -r _; echo inf; }; echo x >&"${LF[1]}"; read -r z <&"${LF[0]}"; echo "f: $z"; wait $LF_PID; }
f; echo "fw=$?"
coproc E { read -r _; echo err >&2; } 2>&1; echo x >&"${E[1]}"; read -r e <&"${E[0]}"; echo "e: $e"; wait $E_PID
coproc head -n 1; echo direct >&"${COPROC[1]}"; read -r d <&"${COPROC[0]}"; echo "d: $d"; wait
coproc Z { read -r _; (( 0 )); }; zp=$Z_PID; echo x >&"${Z[1]}"; wait $zp; echo "zw=$?"
coproc { read -r _; kill -TERM $BASHPID; }; kp=$COPROC_PID; echo x >&"${COPROC[1]}"; wait $kp; echo "kw=$?"
type coproc; compgen -k | tail -2
echo done
