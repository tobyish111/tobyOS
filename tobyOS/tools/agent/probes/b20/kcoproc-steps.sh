# kcoproc-steps.sh: coprocesses, one command line per step (shparity 245a
# 245b 312) -- the values tools/agent/probes/b20/kcoproc.py wants of the
# KERNEL shell, measured here under the guest's bash and /bin/tsh. Every
# output line starts with its step's marker.
exec 2>&1
coproc cat; echo "c1 fds=${COPROC[@]} pid=${COPROC_PID:+set}"
echo hello >&"${COPROC[1]}"; read -r line <&"${COPROC[0]}"; echo "c2 got=$line"
echo "c3 $(declare -p COPROC | sed 's/[0-9][0-9]*/N/g')"
exec {COPROC[1]}>&-; echo "c4 [${COPROC[@]}]"
wait $COPROC_PID; echo "c5 wait=$? [${COPROC[@]-unset}] [${COPROC_PID-unset}]"
coproc NAMED { read x; echo "named got $x"; }; echo "c6 fds=${NAMED[@]}"
echo ping >&"${NAMED[1]}"; read -r r <&"${NAMED[0]}"; echo "c7 $r"
wait $NAMED_PID; echo "c8 w2=$?"
true; echo "c9 [${NAMED[@]-unset}]"
coproc B { cat; }; echo "c10 [${B[@]}]"; eval "exec ${B[1]}>&-"; echo "c10b [${B[@]}]"; eval "exec ${B[0]}<&-"; echo "c10c [${B[@]}]"; wait $B_PID; echo "c10d [${B[@]-unset}]"
coproc { echo x-out; }; sleep 1
read -r r <&"${COPROC[0]}"; echo "c11 r=$r"; wait; echo "c11b [${COPROC[@]-unset}]"
coproc 1bad { :; }; echo "c12 bad=$?"
