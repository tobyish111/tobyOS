# stdinhd.sh: a script read from STDIN -- here-documents, a same-line $( )'s
# body, a body with an apostrophe, `read` taking the next line of the
# script, and the input ending inside a quote, a compound and a body.
# tsh ran `cat <<EOF` with no body and the body's lines as commands, and
# read EOF for good at an open quote (finding 37's stdin side).
exec 2>&1
cd /tmp
t() { printf "$1" > /tmp/sih.sh; $SH < /tmp/sih.sh; echo "rc=$?"; }
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
rm -f /tmp/sih.sh
echo end
