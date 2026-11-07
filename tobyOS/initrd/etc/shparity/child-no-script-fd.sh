# bash reads a script from its descriptor 255, and the children it forks
# have none of it: a ( ), a ( ) inside one, lists 0 1 2 while the script
# itself lists 255 too. Every forked tsh kept its parent's 255 (finding 637).
( ls /proc/$BASHPID/fd > l1; : ); tr '\n' ' ' < l1; echo
( ( ls /proc/$BASHPID/fd > l2; : ); : ); tr '\n' ' ' < l2; echo
( exec 4> /dev/null; ls /proc/$BASHPID/fd > l3; : ); tr '\n' ' ' < l3; echo
ls /proc/$$/fd > l4; tr '\n' ' ' < l4; echo
