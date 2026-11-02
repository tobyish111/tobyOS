# An interactive shell runs PROMPT_COMMAND before each primary prompt, in
# itself: each element of it in turn, with the last command's $?, which it
# then leaves as it was, and its $LINENO the lines read so far; not in a
# shell that is not interactive. tsh had no PROMPT_COMMAND (finding 596).
e=$(mktemp)
printf '%s\n' 'PROMPT_COMMAND='"'"'echo "pc $LINENO"'"'" ':' ':' |
    "$BASH" --norc -i 2>"$e"
printf '%s\n' 'PROMPT_COMMAND=false' 'true' 'echo "st=$?"' | "$BASH" --norc -i 2>"$e"
printf '%s\n' "PROMPT_COMMAND=('echo one' 'echo two \$?')" 'false' ':' |
    "$BASH" --norc -i 2>"$e"
printf '%s\n' 'PROMPT_COMMAND='"'"'x=$((x+1))'"'" 'echo "x=$x"' 'echo "x=$x"' |
    "$BASH" --norc -i 2>"$e"
printf '%s\n' 'PROMPT_COMMAND="echo pc"' 'echo body' | "$BASH" --norc 2>"$e"
rm -f "$e"
