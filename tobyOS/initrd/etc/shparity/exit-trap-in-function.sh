# The EXIT trap runs where exit is called: inside a function it sees that
# function -- $FUNCNAME, the stack, its locals -- and a subshell's exit
# leaves the shell's trap for the shell (finding 384). tsh unwound to the
# top first, and the trap saw neither.
exec 2>&1
x=out
trap 'echo "trap [$x] [$FUNCNAME] [${FUNCNAME[*]}]"' EXIT
f() { local x=in; ( exit 3 ); echo "sub $?"; exit 4; }
g() { f; }
g
echo "not here"
