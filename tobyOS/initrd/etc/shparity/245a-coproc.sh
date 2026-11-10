# coproc [NAME] command: NAME[0]/NAME[1] and NAME_PID, the default name
# COPROC for a simple command, a compound body with a name, `{NAME[1]}>&-`
# and `exec N>&-` really closing the input so the coprocess sees EOF, the
# variables going when it is reaped. tsh ran a program called coproc.
# (batch 12; probe p80 in C:\t\b12)
exec 2>&1
coproc cat
echo "fds=${COPROC[@]} pid=${COPROC_PID:+set}"
echo hello >&"${COPROC[1]}"
read -r line <&"${COPROC[0]}"; echo "got=$line"
declare -p COPROC | sed 's/[0-9][0-9]*/N/g'
exec {COPROC[1]}>&-
wait $COPROC_PID; echo "wait=$?"
echo "after: [${COPROC[@]-unset}] [${COPROC_PID-unset}]"
coproc NAMED { read x; echo "named got $x"; }
echo "named fds=${NAMED[@]}"
echo ping >&"${NAMED[1]}"
read -r r <&"${NAMED[0]}"; echo "$r"
wait $NAMED_PID; echo "w2=$?"
true; echo "after2: [${NAMED[@]-unset}]"
coproc 1bad { :; }; echo "bad=$?"
