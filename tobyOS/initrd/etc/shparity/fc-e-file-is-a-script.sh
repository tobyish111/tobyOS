# fc -e's file runs as a script: each command echoed as it is read and
# recorded as a script's are -- one over lines one entry, a comment one
# too -- history-expanded. tsh ran the file a line at a time, a here-
# document's body lines and the parts of an `if` commands of their own
# (finding 554).
exec 2>&1
unset HISTFILE
set -o history -H
ed() { printf 'echo \041\041 x\n# c\necho one; echo two\n\nif true\nthen echo three\nfi\n' > "$1"; }
cat <<!
four
!
fc -e cat -1
echo =1
fc -e ed -1
fc -l
