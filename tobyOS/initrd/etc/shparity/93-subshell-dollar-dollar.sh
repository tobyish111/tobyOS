# XCU 2.5.2: $$ is the SHELL's pid and does not change in a subshell or a
# command substitution -- both report the PARENT shell. The values differ
# between the two shells, so the case compares INDIRECTLY and prints
# verdicts only.
a=$$
b=$(echo $$)
[ "$a" = "$b" ] && echo "1=same" || echo "1=DIFF"
c=$( ( echo $$ ) )
[ "$a" = "$c" ] && echo "2=same" || echo "2=DIFF"
( [ "$$" = "$a" ] && echo "3=same" || echo "3=DIFF" )
case $PPID in ''|*[!0-9]*) echo "4=bad";; *) echo "4=num";; esac
echo end
