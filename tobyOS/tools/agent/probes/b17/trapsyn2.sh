# trapsyn2.sh: where a trap action's syntax error is numbered and what the
# shell does after it, in a SCRIPT (finding 36). bash runs DEBUG, RETURN
# and ERR actions on from the current line and EXIT and signal actions from
# line 1; a multi-line action runs its lines up to the bad one. The shell's
# own name and the script's path are taken out.
exec 2>&1
f=${TMP:-/tmp}/trapsyn2.$$.sh
r() { $SH "$f" 2>&1 | sed "s#$f#SCRIPT#; s/^[a-z]*: /SH: /"; echo "rc=${PIPESTATUS[0]}"; }
printf '\n\ntrap "done" ERR\n\nfalse\necho "st=$?"\n' > "$f"; r
printf 'f() {\n  :\n  :\n}\ntrap "esac" RETURN\n\nf\necho "st=$?"\n' > "$f"; r
printf 'trap "then" USR1\n\n\nkill -USR1 $$\necho "st=$?"\n' > "$f"; r
printf '\n\ntrap "if then" EXIT\n\necho x\nexit 4\n' > "$f"; r
printf 'trap '"'"'echo ok\nif then'"'"' EXIT\necho y\n' > "$f"; r
printf 'trap '"'"'echo d1\nfi'"'"' DEBUG\n\necho z\n' > "$f"; r
printf 'eval '"'"'echo e1\nfi'"'"'\necho "st=$?"\n' > "$f"; r
printf 'trap "echo L=\\$LINENO" DEBUG\n\n\necho w\n' > "$f"; r
printf 'false\ntrap "if then" EXIT\n' > "$f"; r
printf 'trap "fi" DEBUG\nfalse\necho "st=$?"\n' > "$f"; r
printf 'set -e\ntrap "fi" ERR\nfalse\necho "not here"\n' > "$f"; r
printf 'trap "fi" EXIT\nset -e\nfalse\n' > "$f"; r
printf 'set -e\ntrap "exit 7" ERR\nfalse\necho no\n' > "$f"; r
printf '\n\ntrap '"'"'echo a=$LINENO\necho b=$LINENO'"'"' EXIT\necho x\n' > "$f"; r
printf '\n\ntrap "echo e=\\$LINENO" EXIT\n\necho x\n' > "$f"; r
printf '\ntrap '"'"'echo a=$LINENO\necho b=$LINENO'"'"' DEBUG\n\necho x\n' > "$f"; r
printf 'trap '"'"'cat <<E\nhd-in-trap\nE'"'"' EXIT\necho x\n' > "$f"; r
printf 'f() { trap '"'"'echo r1\nfi'"'"' RETURN; :; }\n\nf\necho "st=$?"\n' > "$f"; r
rm -f "$f"
echo end
