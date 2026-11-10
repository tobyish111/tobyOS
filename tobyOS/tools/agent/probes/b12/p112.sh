sleep 2 & { jobs; echo "bg-group done"; } & wait $!
( jobs; echo "subshell done" )
{ jobs; echo "pipe done"; } | cat
coproc { jobs; echo "coproc done"; }; cat <&"${COPROC[0]}"
echo "cs=[$(jobs)]"
cat <(jobs; echo "ps done")
kill %1 2>/dev/null; wait
