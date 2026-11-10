# fc leaves its own line out of the range only when it is in the history:
# with HISTIGNORE='fc*' it is not, and `fc -l` lists the newest real entry
# -- tsh took one entry away at every fc, and a listing never removes one
# (finding 522).
exec 2>&1
f=${TMP:-/tmp}/fcol.$$
printf '%s\n' 'echo one' 'echo two' 'echo three' > "$f"
HISTFILE=$f
HISTIGNORE='history*:fc*'
set -o history
fc -l
fc -nl
fc -lr
history
HISTIGNORE='history*'
fc -l -2
history
rm -f "$f"
