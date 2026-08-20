# A TRAP IN /bin/tsh HAD NOTHING BEHIND IT.
#
# shell_deliver_signal() is called from src/signal.c -- the KERNEL -- so the
# in-kernel shell's traps fired and the hosted shell's did not. A signal sent
# to /bin/tsh took the default action instead: USR1 killed the shell mid
# script. `trap ... INT` in a script was decoration.
trap 'echo GOT-URG' URG
kill -URG $$
echo A=$?
trap 'echo GOT-USR1' USR1
kill -USR1 $$
echo B=$?
trap - URG USR1
kill -URG $$
echo C=$?
echo D end
