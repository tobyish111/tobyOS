# faultsig.sh: the EXIT trap and the "fault" signals -- ILL TRAP ABRT FPE
# BUS SEGV SYS -- sent with kill, to a shell with an EXIT trap, in the
# script itself, in a subshell, while it waits for a child, and with the
# signal trapped (finding 79's remainder: tsh died without the trap).
exec 2>&1
SH=${SH:-bash}
for s in ILL TRAP ABRT FPE BUS SEGV SYS; do
  $SH -c 'trap "echo exit-trap \$?" EXIT; kill -'$s' $$; echo after'
  echo "f1 $s st=$?"
done
for s in SEGV FPE; do
  $SH -c 'trap "echo exit-trap" EXIT; ( trap "echo sub-trap" EXIT; kill -'$s' $BASHPID; echo sub-after ); echo "sub st=$?"'
  echo "f2 $s st=$?"
  $SH -c 'trap "echo exit-trap" EXIT; sleep 2 & p=$!; ( sleep 0.5; kill -'$s' $$ ) & wait $p; echo after-wait'
  echo "f3 $s st=$?"
  $SH -c 'trap "echo caught" '$s'; trap "echo exit-trap" EXIT; kill -'$s' $$; echo after'
  echo "f4 $s st=$?"
done
$SH -c 'kill -SEGV $$; echo after'
echo "f5 no-trap st=$?"
echo end
