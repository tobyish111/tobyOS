# fc -s takes every PAT=REP before the command, in order, each replacing
# every match: `fc -s a=x` on `echo aa ab ac` runs `echo xx xb xc` -- tsh
# took one pair and its first match (finding 524).
exec 2>&1
set -o history
HISTIGNORE='fc*'
echo aa ab ac
fc -s a=x
fc -s x=4 b=8
fc -s 4=q echo
