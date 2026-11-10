# trapsyn.sh: a syntax error in a trap's action (finding 36). bash reads
# each action with parse_and_execute under the trap's own name -- "exit
# trap", "debug trap", "error trap", "return trap", "trap" for a signal --
# reports the error with the action's text, and the shell goes on. tsh
# named nothing, printed no text and its -c shell stopped. The shell's
# own name is taken out.
exec 2>&1
N() { sed 's/^[a-z]*: /SH: /'; }
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
$SH "$f" 2>&1 | sed "s#$f#SCRIPT#; s/^[a-z]*: /SH: /"; echo "rc=${PIPESTATUS[0]}"
printf 'trap "fi" DEBUG\necho a\necho b\n' > "$f"
$SH "$f" 2>&1 | sed "s#$f#SCRIPT#; s/^[a-z]*: /SH: /"; echo "rc=${PIPESTATUS[0]}"
rm -f "$f"
echo end
