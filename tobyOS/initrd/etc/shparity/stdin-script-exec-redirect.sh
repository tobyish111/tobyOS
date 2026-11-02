# A script read from standard input goes on from wherever `exec 0<` points
# the shell's fd 0: the rest of the script is then that file's, and
# `exec 0<&-` ends it -- tsh read on in the first input (finding 591).
d=$(mktemp -d)
printf 'echo in two\n' > "$d/two"
printf 'echo in one\nexec 0< %s/two\necho not reached\n' "$d" | "$BASH"; echo "rc=$?"
printf 'echo one\nexec 0< %s/two\necho not reached\n' "$d" > "$d/one"
"$BASH" < "$d/one"; echo "rc=$?"
printf 'echo before\nexec 0<&-\necho not reached\n' | "$BASH"; echo "rc=$?"
printf 'read x\nthis is data\necho "x=$x"\n' | "$BASH"
rm -rf "$d"
