# fc's 0 is the newest entry before fc's own line and -0 that line itself,
# in a listing -- to run again -0 names nothing, "no command found" -- and
# a listing with nothing before its line lists that line: tsh took its own
# line out of the list and found neither (finding 546).
exec 2>&1
unset HISTFILE
set -o history
fc -l
echo a
echo b
fc -l 0
fc -l -0
fc -l 0 -0
fc -l 1 0
echo c
fc -s 0
fc -s -0; echo "rc=$?"
