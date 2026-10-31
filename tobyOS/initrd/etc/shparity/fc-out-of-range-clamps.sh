# A number outside fc's list is moved into it: -N before the oldest is the
# oldest; N before the oldest or past the newest is the oldest as a
# range's first and the newest as its last or to run again -- and so is
# the newest's own number. tsh said "history specification out of range"
# (finding 547).
exec 2>&1
unset HISTFILE
set -o history
HISTSIZE=4
history -c
echo a
echo b
echo c
echo d
fc -l 99
echo =1
fc -l 1 99
echo =2
fc -l -20 -1
echo =3
fc -l -40 -20
echo =4
fc -l 1 2
echo =5
fc -e cat 14
echo =6
fc -e cat 99
echo =7
fc -s 99
