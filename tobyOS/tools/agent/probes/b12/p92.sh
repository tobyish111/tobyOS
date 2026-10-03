coproc { sleep 0.2; echo x; }; sleep 0.6; echo "a=${COPROC_PID:+set}"; true; echo "b=${COPROC_PID:+set}"; /bin/true; echo "c=${COPROC_PID:+set} ${COPROC[0]:+fd}"
coproc N { sleep 0.2; }; sleep 0.6; echo "d=${N_PID:+set}"; ( : ); echo "e=${N_PID:+set}"
coproc M { sleep 0.2; }; sleep 0.6; echo "f=${M_PID:+set}"; x=$(echo hi); echo "g=${M_PID:+set}"; :|:; echo "h=${M_PID:+set}"
coproc K { sleep 0.2; }; sleep 0.6; echo "i=${K_PID:+set}"; sleep 0 & echo "j=${K_PID:+set}"; wait
coproc L { sleep 0.2; echo z; }; sleep 0.6; read -r w <&"${L[0]}"; echo "w=$w"; /bin/true; echo "k=${L_PID:+set}"
f() { :; }; coproc R { sleep 0.2; }; sleep 0.6; f; echo "m=${R_PID:+set}"; f | cat; echo "n=${R_PID:+set}"
coproc S { sleep 0.2; }; sleep 0.6; echo "o=${S_PID:+set}"; { :; } > /dev/null; echo "p=${S_PID:+set}"; cat < <(echo ps); echo "q=${S_PID:+set}"
coproc T { sleep 0.2; }; sleep 0.6; echo "r=${T_PID:+set}"; exec 9>/dev/null; echo "s=${T_PID:+set}"; jobs; echo "t=${T_PID:+set}"
echo end
