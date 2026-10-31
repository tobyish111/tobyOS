# The command fc -s runs again is not a line the shell read: an fc in it
# does not take the entry fc -s made for its own line, so `fc -s 'fc -l'`
# lists through that entry -- tsh's stopped short of it (finding 553).
exec 2>&1
unset HISTFILE
set -o history
echo a
fc -l 'echo'
echo =1
fc -s 'fc -l'
fc -s 'fc -l'
