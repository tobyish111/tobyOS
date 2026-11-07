# The redirections of the command a child is forked for -- a ( )'s own, an
# async command's, a pipeline element's -- are applied for good: their
# files are the child's 0-2 and nothing they replaced is kept. An inner
# command's are undone as anywhere, a saved copy from 10 up while they
# last, and so are a coprocess's own. tsh kept a copy for both (finding
# 639).
( ls /proc/$BASHPID/fd > l1; : ) 2> /dev/null; tr '\n' ' ' < l1; echo
{ ls /proc/$BASHPID/fd > l2; :; } 2> /dev/null & wait; tr '\n' ' ' < l2; echo
{ ls /proc/$BASHPID/fd > l3; :; } 2> /dev/null | cat; tr '\n' ' ' < l3; echo
for i in 1; do ls /proc/$BASHPID/fd > l4; :; done 2> /dev/null & wait; tr '\n' ' ' < l4; echo
f() { ls /proc/$BASHPID/fd > l5; :; }; f 2> /dev/null & wait; tr '\n' ' ' < l5; echo
echo x | { ls -l /proc/$BASHPID/fd/0 > l6; :; } < /dev/null; sed 's/.*-> //' l6
( { ls /proc/$BASHPID/fd > l7; :; } 2> /dev/null; : ); tr '\n' ' ' < l7; echo
coproc { ls /proc/$BASHPID/fd > l8; :; } 2> /dev/null; wait; tr '\n' ' ' < l8; echo
{ ( ls /proc/$BASHPID/fd > l9; : ) 2> /dev/null; } > /dev/null; tr '\n' ' ' < l9; echo
