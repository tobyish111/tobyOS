# `set -o history` turning history on in a script reads $HISTFILE when the
# session has no lines yet, so `history` lists the file's lines -- tsh's
# list stayed empty (finding 521).
exec 2>&1
f=${TMP:-/tmp}/sohl.$$
printf '%s\n' 'echo first' 'ls -l' 'echo third' > "$f"
HISTFILE=$f
HISTIGNORE='history*'
set -o history
history
echo added
history
rm -f "$f"
