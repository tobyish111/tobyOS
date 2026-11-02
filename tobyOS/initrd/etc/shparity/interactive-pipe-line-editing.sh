# An interactive shell reading a pipe edits its lines as bash's readline
# does there: ^R searches the history (here a history file's, read at
# start), any other control key ends the search and runs on the line found
# -- ^O runs it and fetches the next -- and ^P walks back; plain lines are
# still echoed after the prompt on stderr. tsh read the keys as text
# (finding 599).
d=$(mktemp -d)
export HISTFILE=$d/h
printf '%s\n' 'echo 0' 'echo 1' 'echo "(left' 'mid' 'right)"' 'echo A' 'echo B' > "$HISTFILE"
printf 'HISTFILE=\n\022left\017\017\017\017\n' | HISTSIZE= "$BASH" --norc -i 2>"$d/e"
printf 'HISTFILE=\n\022left\017\017\017\017\n' | HISTSIZE=8 "$BASH" --norc -i 2>"$d/e"
unset HISTFILE
printf 'echo 0\necho "(left\nmid\nright)"\necho A\necho B\n\020\020\020\017\017\n' |
    HISTSIZE=6 HISTFILE= "$BASH" --norc -i 2>"$d/e"
printf 'echo one\necho two\n\022one\n\020\n' | HISTFILE= "$BASH" --norc -i 2>"$d/e"
printf 'echo hi\n' | HISTFILE= PS1='P> ' "$BASH" --norc -i 2>&1 >/dev/null | grep -c 'P> echo hi'
rm -rf "$d"
