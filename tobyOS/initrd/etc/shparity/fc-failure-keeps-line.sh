# An fc that fails keeps its line in the list: it goes once -s has found
# its command, or -e starts to edit -- an editor that fails, or nothing
# to edit, too. tsh took the line out before it looked (finding 550).
exec 2>&1
unset HISTFILE
set -o history
fc -s; echo "rc=$?"
echo a
fc -e cat -0
fc -s -0
fc -s nosuch
fc -x
fc -e
fc -e false 1; echo "rc=$?"
fc -l
history -c
fc -e cat; echo "rc=$?"
fc -l
