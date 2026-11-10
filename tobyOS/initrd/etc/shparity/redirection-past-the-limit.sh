# A redirection to a descriptor at or past the soft open-file limit: bash
# makes the file and its dup2 to the number fails, "N: Bad file
# descriptor", status 1, the command not run -- a dup naming its target --
# and a number past the descriptor table is the same. {NAME} with no
# number free from 10 up to the limit is "redirection error: cannot
# duplicate fd: ..." and the file's name with it. No descriptor free at
# all is "No file descriptors available". A descriptor opened before the
# limit fell stays open (finding 649).
exec 2>&1
rm -f new200
exec 2000>/dev/null; echo "past the table: $?"
ulimit -n 128
exec 200>/dev/null; echo "exec 200: $?"
echo hi 200>/dev/null; echo "echo 200: $?"
cat /dev/null 130</dev/null; echo "cat 130: $?"
{ echo in; } 140>/dev/null; echo "group 140: $?"
echo a 200>&1; echo "dup 200: $?"
exec 200>new200; echo "made: $?"; ls new200
exec 127>/dev/null; echo x >&127; echo "127: $?"; exec 127>&-
(ulimit -n 12; exec {v}>/dev/null; echo "v=$v"; exec {w}>/dev/null; echo "w=$w: $?")
(ulimit -n 10; exec {v}>&1; echo "v=$v: $?")
(ulimit -n 6; exec 3>/dev/null 4>/dev/null 5>/dev/null; echo x > f6; echo "full: $?")
