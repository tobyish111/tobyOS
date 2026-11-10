# A background job a signal ended is reported when the wait for a
# FOREGROUND command ends -- a program, a ( ), a pipeline -- before that
# command's own report, naming bash's line_number: the line a function's
# body starts on, a for's head, a while's last line. A ( ), a compound
# pipeline stage and a background group report their own jobs. tsh
# reported only after a program and at the next line, with $LINENO. Run
# as a script of its own, pids replaced.
# (batch 12; probes p93 p102 p105 p111 in C:\t\b12)
exec 2>&1
cat > nj.sh <<'EOF'
sleep 1 & kill -9 $!; sleep 0.3; echo "after program"
sleep 1 & kill -9 $!; ( sleep 0.3 ); echo "after subshell"
sleep 1 & kill -9 $!; sleep 0.3 | cat; echo "after pipeline"
f() {
  echo "in f"
  sh -c 'kill -9 $$'
}
f
for i in 1; do
  sleep 1 & kill -9 $!; sleep 0.3
done
k=0
while (( k < 1 )); do
  sleep 1 & kill -9 $!; sleep 0.3
  k=1
done
sleep 1 & kill -9 $!; sleep 0.3; sh -c 'kill -9 $$'; echo "both"
( sleep 1 & kill -9 $!; sleep 0.3; echo "a subshell reports its own" )
{ sleep 1 & kill -9 $!; sleep 0.3; echo "so does a stage"; } | cat
{ sleep 1 & kill -9 $!; sleep 0.3; echo "and a background group"; } & wait
echo end
EOF
$BASH nj.sh 2>&1 | sed 's/: *[0-9][0-9]* Killed/: PID Killed/'
