# cpclose.sh: closing a coprocess's descriptors from the shell -- by {var}
# and by number -- and what NAME, NAME_PID and the next coprocess see.
exec 2>&1
coproc cat
echo "1 [${COPROC[@]}]"
exec {COPROC[1]}>&-
echo "2 [${COPROC[@]}] pid=${COPROC_PID:+set}"
wait $COPROC_PID; echo "3 wait=$? [${COPROC[@]-unset}]"
coproc B { cat; }
echo "4 [${B[@]}]"
eval "exec ${B[1]}>&-"
echo "5 [${B[@]}]"
eval "exec ${B[0]}<&-"
echo "6 [${B[@]}]"
wait $B_PID; echo "7 wait=$? [${B[@]-unset}]"
coproc C { read x; echo "c $x"; }
echo "8 [${C[@]}]"
echo hi >&"${C[1]}"; read -r l <&"${C[0]}"; echo "9 $l"
wait $C_PID; echo "10 [${C[@]-unset}]"
echo end
