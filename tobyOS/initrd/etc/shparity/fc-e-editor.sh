# fc -e ENAME puts the lines into a file, runs ENAME on it, then echoes and
# runs what the file holds; an editor that fails runs nothing -- tsh
# refused, "no editor available" (finding 526).
exec 2>&1
set -o history
HISTIGNORE='fc*'
echo aa
echo bb
echo cc
fc -e cat
echo dd
fc -e cat -2 -1
fc -e false; echo "rc=$?"
