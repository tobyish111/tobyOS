# A job that ended is noticed quietly (bash's SIGCHLD) and reported only
# at a notify: `jobs` lists "Killed" and nothing reports it, `wait $!`
# reports it, a plain `wait` with nothing to wait for keeps $!'s for the
# next line, `jobs -p` marks nothing; a job started meanwhile takes the
# number a listed one freed, and a dead job is never the `-` one. tsh
# reported a death where it reaped it. Pids replaced.
# (batch 12; probe p104 in C:\t\b12)
exec 2>&1
cat > nj.sh <<'EOF'
busy() { e=$((SECONDS+2)); while ((SECONDS<e)); do :; done; }
sleep 1 & kill -9 $!; busy; jobs; echo "a after jobs"
echo "a next"
sleep 1 & kill -9 $!; busy; wait $!; echo "b w=$?"
echo "b next"
sleep 1 & kill -9 $!; busy; wait; echo "c w=$?"
echo "c next"
sleep 1 & kill -9 $!; busy; jobs -p >/dev/null; echo "d after jobs -p"
echo "d next"
sleep 1 & kill -9 $!; busy; sleep 0.5 & echo "e started"; wait
echo "e next"
sleep 0.1 & busy; sleep 2 & jobs; kill %2; wait
echo "f next"
sleep 0.1 & sleep 0.3; jobs; sleep 1 & jobs; kill %%; wait
echo end
EOF
$BASH nj.sh 2>&1 | sed 's/: *[0-9][0-9]* Killed/: PID Killed/'
