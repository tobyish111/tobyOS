# chldtrap.sh: SIGCHLD stays the shell's own whatever is trapped (bash's
# SIG_NO_TRAP). tsh made `trap '' CHLD` an ignore of the signal itself and
# `trap - CHLD` its default -- and once the shell learns of its jobs from
# SIGCHLD (finding 44) either would leave it deaf to them.
exec 2>&1
trap '' CHLD; sleep 0.1 & wait $!; echo "c1 $?"
sleep 0.1 & sleep 0.5; jobs; echo "c1b"
trap - CHLD; sleep 0.1 & wait $!; echo "c2 $?"
sleep 0.1 & sleep 0.5; jobs; echo "c2b"
trap 'echo trapped' CHLD; sleep 0.1; trap - CHLD; echo "c3"
( sleep 0.1 & wait $!; echo "c4 $?" )
trap '' CHLD; ( sleep 0.1 & sleep 0.5; jobs; echo "c5" ); trap - CHLD
sleep 0.2 & kill -9 $!; sleep 0.5; jobs; echo "c6"
x=$(sleep 0.1 & wait $!; echo "c7 $?"); echo "$x"
echo end
