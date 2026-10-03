exec 2>&1
sleep 0.1 & sleep 0.3; jobs; sleep 1 & jobs; kill %%; wait
echo "--"
sleep 0.1 & sleep 0.3; jobs; true; jobs
echo "-- next line"
jobs
echo "--"
coproc K { sleep 0.2; }; e=$((SECONDS+2)); while (( SECONDS < e )); do :; done; echo "j=${K_PID:+set}"; sleep 0 & echo "k=${K_PID:+set}"; wait
coproc M { sleep 0.2; }; e=$((SECONDS+2)); while (( SECONDS < e )); do :; done; echo "l=${M_PID:+set}"; x=$(:); echo "m=${M_PID:+set}"; cat < <(:); echo "n=${M_PID:+set}"
echo "o=${M_PID:+set}"
echo end
