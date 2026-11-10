# trap "" SIG is an IGNORE (bash: SIG_IGN): children inherit it and exec
# keeps it; a caught signal is reset to the default in an exec'd child;
# `trap -` puts the old disposition back; a subshell shows its parent's
# traps but runs only ignores; a trapped signal is reported bare.
exec 2>&1
trap "" USR1
sh -c 'kill -USR1 $$; echo "ignored: child alive"'; echo "  status=$?"
( trap -p USR1 ); echo "  subshell trap -p status=$?"
n=0 i=0
while [ $i -lt 30 ]; do
  sleep 3 & p=$!; kill -USR1 $p; sleep 0.1; kill -TERM $p; wait $p
  [ $? = 143 ] && n=$((n+1)); i=$((i+1))
done 2>/dev/null
echo "ignored across exec: TERM $n/30"
trap 'echo caught' USR1
sh -c 'kill -USR1 $$; echo "caught: child alive"'; echo "  status=$?"
( trap -p USR1 ); echo "  subshell trap -p status=$?"
echo "cmdsub sees: [$(trap -p USR1)]"
( trap 'echo inner' USR2; trap -p USR1; echo "  after a trap of its own: [$(trap -p USR2)]" )
# (Once it is sleep: before its exec the child still has bash's handler.)
sleep 3 & p=$!; sleep 0.2; kill -USR1 $p; wait $p; echo "  background, trapped: status=$?"
trap - USR1
# (Its report names a pid and the command's source text: kept out.)
{ sh -c 'kill -USR1 $$; echo "reset: child alive"'; } 2>/dev/null; echo "  status=$?"
trap -p USR1; echo "trap -p after reset status=$?"
echo done
