# bash's reader notifies each time it takes a line -- of the script, of
# a sourced file (its line 1), of an eval string (the eval's line), of a
# signal trap's action (line 1), DEBUG and ERR actions (the current line)
# -- and as it parses each $( ) and <( ) again to expand it, one line past
# the command's own; not for backquotes or $(( )). tsh reported a job
# killed before them at the next line. Pids replaced.
# (batch 12; probes p99 p106 in C:\t\b12)
exec 2>&1
cat > nj.sh <<'EOF'
dead() { sleep 1 & kill -9 $!; e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
dead; echo "A"; x=$(echo a); echo "A [$x]"
dead; echo "B"; x=$(echo b
echo b2); echo "B [$x]"
dead; echo "C"; x=`echo c`; echo "C [$x]"
dead; echo "D"; eval 'echo d'; echo "D after"
dead; echo "F"; cat <(echo f); echo "F after"
dead; echo "G"; echo 'echo g' > src.sh; . ./src.sh; echo "G after"
dead; echo "H"; trap 'echo usr1' USR1; kill -USR1 $$; echo "H after"
dead; echo "I"; trap 'true' DEBUG; echo "I debug"; trap - DEBUG
dead; echo "J"; trap 'true' ERR; false; echo "J err"; trap - ERR
dead; echo "K"; x=$(( 1 + 2 )); echo "K after"
dead; echo "L"; [[ $(echo l) ]]; echo "L after"
dead; echo "M"; true; echo "M after"
echo end
EOF
$BASH nj.sh 2>&1 | sed 's/: *[0-9][0-9]* Killed/: PID Killed/'
