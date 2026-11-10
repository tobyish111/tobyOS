# fc -s puts the command it runs again in place of the newest entry: where
# HISTIGNORE or ignorespace kept fc's own line out of the list, that is the
# entry before it, which goes all the same, and the command is recorded
# under the same rules -- tsh kept the entry and added the command after it
# (finding 556).
exec 2>&1
unset HISTFILE
set -o history
HISTIGNORE='fc*'
echo a
echo b
fc -s b=c
fc -l
fc -e - c=d echo
fc -l
HISTIGNORE='fc*:echo e'
fc -s d=e
fc -l
HISTIGNORE=
HISTCONTROL=ignorespace
echo f
 fc -s f=g
fc -l
