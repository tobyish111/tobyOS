# A coprocess that has ended is disposed -- NAME and NAME_PID unset, its
# descriptors closed -- at bash's cleanups: the end of a foreground wait,
# the start of a job, each loop iteration. tsh kept it until the next line.
# (batch 12; probes p92 p96 p97 in C:\t\b12)
exec 2>&1
coproc { sleep 0.2; echo x; }; sleep 0.6; echo "a=${COPROC_PID:+set}"; true; echo "b=${COPROC_PID:+set}"
coproc N { sleep 0.2; }; sleep 0.6; echo "d=${N_PID:+set}"; ( : ); echo "e=${N_PID:+set}"
coproc L { sleep 0.2; echo z; }; sleep 0.6; read -r w <&"${L[0]}"; echo "w=$w"
coproc K { sleep 0.2; }; e=$((SECONDS+2)); while (( SECONDS < e )); do :; done; echo "j=${K_PID:+set}"
coproc M { sleep 0.2; }; e=$((SECONDS+2)); until (( SECONDS >= e )); do true; done; echo "h=${M_PID:+set}"
coproc P { sleep 0.2; }; for ((s=SECONDS+2; SECONDS < s; )); do :; done; echo "p=${P_PID:+set}"
jobs
echo end
