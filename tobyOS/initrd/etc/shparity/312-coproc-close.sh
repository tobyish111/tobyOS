# A REDIRECTION THAT CLOSES A COPROCESS'S DESCRIPTOR TAKES IT OFF THE
# COPROCESS: after `{COPROC[1]}>&-`, `exec 60>&-` or `exec 63<&-` the
# element says -1, and the next coprocess takes 63 and 60 again. tsh kept
# the numbers, skipped the closed slots (62 58), and under the stress
# build gave 245a's second coprocess 59 -- its reap compared a dangling
# pointer (finding 115; logs/b18 cpclose).
# Each coprocess lives a second past its cat: a closed input ends cat at
# once, and bash disposes of a coprocess whenever it reaps it -- in the
# batch 22 gate bash's was gone by line 6 ("6 []") and tsh's not yet, and
# 20 runs over SSH had both at [-1 -1]. Alive, the lines before each wait
# show the closed ends, as they were meant to, in both shells.
exec 2>&1
coproc { cat; sleep 1; }
echo "1 [${COPROC[@]}]"
exec {COPROC[1]}>&-
echo "2 [${COPROC[@]}] pid=${COPROC_PID:+set}"
wait $COPROC_PID; echo "3 wait=$? [${COPROC[@]-unset}]"
coproc B { cat; sleep 1; }
echo "4 [${B[@]}]"
eval "exec ${B[1]}>&-"
echo "5 [${B[@]}]"
eval "exec ${B[0]}<&-"
echo "6 [${B[@]}]"
wait $B_PID; echo "7 [${B[@]-unset}]"
coproc C { read x; echo "c $x"; }
echo "8 [${C[@]}]"
echo hi >&"${C[1]}"; read -r l <&"${C[0]}"; echo "9 $l"
wait $C_PID; echo "10 [${C[@]-unset}]"
echo end
