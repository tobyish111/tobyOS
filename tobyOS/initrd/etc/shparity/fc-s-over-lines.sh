# fc -s reads the command it runs again as shell input, under the name fc
# -- "fc: line N: syntax error" -- so an entry over lines, a here-
# document's, runs as the one command it is: tsh ran it as one line, the
# body's words cat's operands (finding 552).
exec 2>&1
unset HISTFILE
set -o history
cat <<!
one
two
!
fc -s cat
fc -s one=1 cat
echo if then fi
fc -s 'echo '=; echo "rc=$?"
