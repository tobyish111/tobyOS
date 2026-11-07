# A forked child's 0, 1 and 2 are where its input and output go -- a
# pipeline element's pipe, a $( )'s or a <( )'s, an async command's
# /dev/null, a coprocess's pipes -- as bash's child has them. tsh's child
# pointed the shell's idea of 0-2 at them and kept its parent's descriptors,
# the pipe at a number of its own (finding 638).
{ ls /proc/$BASHPID/fd > l1; :; } | cat; tr '\n' ' ' < l1; echo
x=$(ls /proc/$BASHPID/fd > l2; :); tr '\n' ' ' < l2; echo
{ ls /proc/$BASHPID/fd > l3; :; } & wait; tr '\n' ' ' < l3; echo
coproc { ls /proc/$BASHPID/fd > l4; :; }; wait; tr '\n' ' ' < l4; echo
f() { ls /proc/$BASHPID/fd > l5; :; }; f | cat; tr '\n' ' ' < l5; echo
{ ls -l /proc/$BASHPID/fd/1 > l6; :; } | cat; sed 's/.*-> //; s/:.*//' l6
{ ls -l /proc/$BASHPID/fd/0 > l7; :; } & wait; sed 's/.*-> //' l7
cat <(ls /proc/$BASHPID/fd > l8; :) > /dev/null; tr '\n' ' ' < l8; echo
