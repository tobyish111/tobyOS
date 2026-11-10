# A SCRIPT READ FROM STANDARD INPUT is one script, read a command at a time
# as bash reads it: a here-document's body comes from the lines after it
# (and a same-line $( )'s), `read` takes the script's next line, a lone
# program is not exec'd in place, the EXIT trap runs once at the end, lines
# count from the start, and input that ends inside a quote, a compound or a
# body is reported, not waited on for good. tsh ran each line as a -c
# string of its own (logs/b17 stdinhd). The shell's name is taken out.
exec 2>&1
# the shell under test: the gate sets no SH (probe3 does); $BASH is its own path
SH=${SH:-$BASH}
d=${TMP:-/tmp}; f=$d/fx300.$$.sh
t() { printf "$1" > "$f"; $SH < "$f" 2>&1 | sed 's|^[/a-z]*: line|SH: line|'; echo "rc=${PIPESTATUS[0]}"; }
t 'cat <<EOF\nab\nEOF\necho after\n'
t 'x=$(cat <<EOF)\nhello\nEOF\necho "[$x]"\n'
t 'cat <<EOF\nit'"'"'s\nEOF\necho after2\n'
t 'read v\nfrom-script\necho "v=$v"\n'
t 'cat <<A; cat <<B\none\nA\ntwo\nB\n'
t 'if true; then cat <<E\nin-if\nE\nfi\n'
t 'echo a\n/bin/echo b\necho c\n'
t 'trap "echo bye" EXIT\necho one\necho two\n'
t 'echo a\nif then\necho b\n'
t 'trap "echo bye2" EXIT\necho x\nexit 3\necho never\n'
t 'echo $LINENO\n\n# c\necho $LINENO\nnosuch_cmd_z\n'
t 'echo "abc\n'
t 'if true; then\necho in\n'
t 'cat <<EOF\nnever ended\n'
rm -f "$f"
echo end
