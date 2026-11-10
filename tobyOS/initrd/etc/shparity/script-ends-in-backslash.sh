# A script file whose last bytes are a backslash ends in a continuation, and
# the backslash goes: `echo AAA\` prints AAA; with a newline after it too
# (finding 445).
exec 2>&1
d=$(mktemp -d)
printf 'echo AAA\\' > "$d/a.sh"; "$BASH" "$d/a.sh"
printf 'echo BBB\\\n' > "$d/b.sh"; "$BASH" "$d/b.sh"
printf 'echo CCC \\' > "$d/c.sh"; "$BASH" "$d/c.sh"
rm -rf "$d"
