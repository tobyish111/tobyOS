# fc's word for a text spec that names no entry is bash's, "no command
# found" -- tsh said "no such history entry" (finding 525). A number past
# the end bash clamps; that is left out here.
exec 2>&1
set -o history
HISTIGNORE='fc*'
echo one
echo two
fc -l nosuch; echo "rc=$?"
fc -s nosuch; echo "rc=$?"
fc -l 1 nosuch; echo "rc=$?"
