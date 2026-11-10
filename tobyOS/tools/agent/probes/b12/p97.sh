coproc K { sleep 1; }; echo "a=${K_PID:+set} ${K[0]:+fd}"; e=$((SECONDS+2)); while (( SECONDS < e )); do :; done; echo "b=${K_PID:+set}"
wait
coproc L { sleep 0.2; }; echo "c=${L_PID:+set}"; e=$((SECONDS+2)); while (( SECONDS < e )); do :; done; echo "d=${L_PID:+set}"
coproc M { sleep 0.2; }; echo "e=${M_PID:+set}"; e=$((SECONDS+2)); while [ $SECONDS -lt $e ]; do :; done; echo "f=${M_PID:+set}"
coproc N { sleep 0.2; }; echo "g=${N_PID:+set}"; e=$((SECONDS+2)); until (( SECONDS >= e )); do true; done; echo "h=${N_PID:+set}"
coproc P { sleep 0.2; }; echo "i=${P_PID:+set}"; for ((s=SECONDS+2; SECONDS < s; )); do :; done; echo "j=${P_PID:+set}"
echo end
