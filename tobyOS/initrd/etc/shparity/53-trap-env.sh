# POSIX 2.11 traps + 2.12 execution environment.
# An EXIT trap fires when the shell (or subshell) leaves, a variable
# assignment PREFIXED to a command affects only that command, and exported
# variables reach children while unexported ones do not.
( trap 'echo "1:exit-trap"' EXIT; echo "1:body" )
( trap 'echo "2:trap"' EXIT; exit 3 ); echo "3=$?"
( trap 'echo "4:trap"' EXIT; false )
( trap - EXIT; echo "5:trap-cleared" )
( trap 'echo "6:outer"' EXIT; ( trap 'echo "6:inner"' EXIT; echo "6:body" ) )

v=original
v=prefixed env > /dev/null 2>&1 || true
echo "7=$v"

show() { echo "8:[$PREF]"; }
PREF=set-for-command show
echo "9:[$PREF]"

export EXPORTED=yes
UNEXPORTED=no
echo "10:$EXPORTED"
echo "11:$UNEXPORTED"

sub() { echo "12:[$EXPORTED][$UNEXPORTED]"; }
sub
( echo "13:[$EXPORTED][$UNEXPORTED]" )

readonly RO=locked
echo "14=$RO"

x=1
( x=2; echo "15-in=$x" )
echo "16-out=$x"

f() { g=set-in-fn; }
f
echo "17=$g"

unset EXPORTED
echo "18=[${EXPORTED-gone}]"
echo end
