# An interactive shell numbers each line it reads on from the ones before:
# the second line's $LINENO is 2, a function called there has BASH_LINENO
# 2, and a compound's lines count too -- tsh began every line at 1 (finding
# 595).
e=$(mktemp)
printf '%s\n' 'echo "a $LINENO"' 'echo "b $LINENO"' '' 'echo "c $LINENO"' |
    "$BASH" --norc -i 2>"$e"
printf '%s\n' 'f() { echo "[${BASH_LINENO[*]}] $LINENO"; }' 'f' ':' 'f' |
    "$BASH" --norc -i 2>"$e"
printf '%s\n' 'if true; then' 'echo "in $LINENO"' 'fi' 'echo "after $LINENO"' |
    "$BASH" --norc -i 2>"$e"
rm -f "$e"
