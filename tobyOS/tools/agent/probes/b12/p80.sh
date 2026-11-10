exec 2>&1
coproc cat
echo "fds=${COPROC[@]} pid=${COPROC_PID:+set}"
echo hello >&"${COPROC[1]}"
read -r line <&"${COPROC[0]}"; echo "got=$line"
declare -p COPROC | sed 's/[0-9][0-9]*/N/g'
ls -l /proc/self/fd 2>/dev/null | grep -c ' 6[0-3] ->'
exec {COPROC[1]}>&-
wait $COPROC_PID; echo "wait=$?"
echo "after: [${COPROC[@]-unset}] [${COPROC_PID-unset}]"
coproc NAMED { read x; echo "named got $x"; }
echo "named fds=${NAMED[@]}"
echo ping >&"${NAMED[1]}"
read -r r <&"${NAMED[0]}"; echo "$r"
wait $NAMED_PID; echo "w2=$?"
true; echo "after2: [${NAMED[@]-unset}]"
coproc sleep 1
coproc cat 2>&1
echo "s=$?"
kill $COPROC_PID 2>/dev/null; wait 2>/dev/null
coproc 1bad { :; }; echo "bad=$?"
