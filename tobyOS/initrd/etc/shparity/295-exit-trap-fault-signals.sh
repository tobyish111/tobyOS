# THE FAULT SIGNALS RUN THE EXIT TRAP TOO -- ILL TRAP ABRT FPE BUS SEGV
# SYS, in bash's terminating_signals: `kill -SEGV $$` with an EXIT trap set
# runs it and the shell dies of SEGV; a subshell runs its own; a wait is
# cut short; a trapped one is caught and the shell goes on. tsh died of
# them without the trap (finding 79's remainder, logs/b16 faultsig). Job
# reports go to /dev/null: they name pids, and their words are the C
# library's.
exec 2>&1
for s in ILL TRAP ABRT FPE BUS SEGV SYS; do
  { "$BASH" -c 'trap "echo exit-'$s' \$?" EXIT; (exit 4); kill -'$s' $$; echo after-'$s; } 2>/dev/null
  echo "$s $?"
done
{ "$BASH" -c 'trap "echo outer" EXIT; ( trap "echo sub" EXIT; kill -SEGV $BASHPID; echo sub-after ); echo "sub st=$?"'; } 2>/dev/null
echo "f2 $?"
{ "$BASH" -c 'trap "echo fexit" EXIT; sleep 2 & p=$!; ( sleep 0.5; kill -FPE $$ ) & wait $p; echo after-wait'; } 2>/dev/null
echo "f3 $?"
"$BASH" -c 'trap "echo caught" BUS; trap "echo bexit" EXIT; kill -BUS $$; echo after-bus'
echo "f4 $?"
{ "$BASH" -c 'kill -ILL $$; echo after'; } 2>/dev/null
echo "f5 $?"
echo end
