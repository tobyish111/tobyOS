# A SYNTAX ERROR IN A TRAP'S ACTION is reported as bash reports it, under
# the trap's name -- "exit trap", "debug trap", "error trap", "return
# trap", "trap" -- on the right line: EXIT and a signal's from 1, DEBUG
# RETURN and ERR on from the current one; the lines before it run, $? is
# what it was, the shell goes on (set -e: it ends, 2). And a RETURN trap
# counts from a `return`'s line, or the body's first when it ran to its
# end; an EXIT trap from 1 (finding 36; logs/b17 trapsyn*).
exec 2>&1
# the shell under test: the gate sets no SH (probe3 does); $BASH is its own path
SH=${SH:-$BASH}
N() { sed 's|^[/a-z]*: |SH: |'; }
c() { $SH -c "$1" 2>&1 | N; echo "rc=${PIPESTATUS[0]}"; }
c 'trap "if then" EXIT; echo body'
c 'trap "echo (" EXIT; echo body2'
c 'trap "fi" DEBUG; echo dbg; echo dbg2'
c 'trap "done" ERR; false; echo "err st=$?"'
c 'f() { :; }; trap "esac" RETURN; f; echo "ret st=$?"'
c 'trap "then" USR1; kill -USR1 $$; echo "usr1 st=$?"'
c 'trap "echo ok; if" EXIT; echo body3'
c 'trap "x=(" EXIT; echo body4'
f=${TMP:-/tmp}/trapsyn.$$.sh
printf 'trap "if then" EXIT\necho in-script\nfalse\n' > "$f"
$SH "$f" 2>&1 | sed "s#$f#SCRIPT#; s|^[/a-z]*: |SH: |"; echo "rc=${PIPESTATUS[0]}"
printf 'trap "fi" DEBUG\necho a\necho b\n' > "$f"
$SH "$f" 2>&1 | sed "s#$f#SCRIPT#; s|^[/a-z]*: |SH: |"; echo "rc=${PIPESTATUS[0]}"
rm -f "$f"
f=${TMP:-/tmp}/trapsyn2.$$.sh
r() { $SH "$f" 2>&1 | sed "s#$f#SCRIPT#; s|^[/a-z]*: |SH: |"; echo "rc=${PIPESTATUS[0]}"; }
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
f=${TMP:-/tmp}/trapsyn3.$$.sh
g=${TMP:-/tmp}/trapsyn3s.$$.sh
r() { $SH "$f" 2>&1 | sed "s#$f#SCRIPT#; s#$g#SOURCED#; s|^[/a-z]*: |SH: |"; echo "rc=${PIPESTATUS[0]}"; }
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  :\n  :\n}\n\nf\necho after\n' > "$f"; r
printf 'f() {\n  trap '"'"'fi'"'"' RETURN\n  :\n  :\n}\n\nf\necho after\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R1=$LINENO\necho R2=$LINENO'"'"' RETURN\n  :\n  :\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo r1\nfi'"'"' RETURN\n  :\n  :\n}\n\nf\n' > "$f"; r
printf 'f() { trap '"'"'echo R=$LINENO'"'"' RETURN; :; }\n\nf\n' > "$f"; r
printf 'f() {\n  :\n  return 3\n  :\n}\ntrap '"'"'echo R=$LINENO'"'"' RETURN\nset -T\n\nf\necho "st=$?"\n' > "$f"; r
printf 'f() {\n  :\n  return 3\n  :\n}\ntrap '"'"'fi'"'"' RETURN\nset -T\n\nf\necho "st=$?"\n' > "$f"; r
printf ':\n:\necho in-sourced\n' > "$g"
printf 'trap '"'"'echo R=$LINENO'"'"' RETURN\n\n. %s\necho after\n' "$g" > "$f"; r
printf 'trap '"'"'fi'"'"' RETURN\n\n. %s\necho after\n' "$g" > "$f"; r
printf 'f() {\n  :\n  false\n  :\n}\nset -E\ntrap '"'"'echo E=$LINENO'"'"' ERR\n\nf\n' > "$f"; r
printf 'f() {\n  :\n  false\n  :\n}\nset -E\ntrap '"'"'fi'"'"' ERR\n\nf\n' > "$f"; r
printf '\n\ntrap '"'"'echo e=$LINENO'"'"' EXIT\n\nf() { exit 3; }\n\nf\n' > "$f"; r
printf 'trap '"'"'echo s=$LINENO'"'"' USR1\n\n\nkill -USR1 $$\n' > "$f"; r
printf '\nf()\n{\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  :\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  for i in 1; do\n    :\n  done\n  :\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  case x in\n    x) : ;;\n  esac\n  :\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  if true; then\n    :\n  fi\n}\n\nf\n' > "$f"; r
printf 'f() {\n  trap '"'"'echo R=$LINENO'"'"' RETURN\n  if true; then\n    return 2\n  fi\n}\n\nf\n' > "$f"; r
printf '\n\ntrap '"'"'nosuch_cmd_q'"'"' EXIT\n\necho x\n' > "$f"; r
printf 'trap '"'"'echo s=$LINENO; nosuch_cmd_s'"'"' USR1\n\n\nkill -USR1 $$\n' > "$f"; r
rm -f "$f" "$g"
echo end
