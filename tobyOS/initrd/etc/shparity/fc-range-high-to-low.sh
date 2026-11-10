# A range fc is given high to low goes high to low, listed or edited, -r
# or not: `fc -l 4 2` lists 4 3 2 -- tsh went low to high unless -r
# (finding 548).
exec 2>&1
unset HISTFILE
set -o history
echo a
echo b
echo c
echo d
echo e
fc -l 4 2
fc -lr 4 2
fc -l -1 -3
fc -nl 3 1
echo =1
fc -e cat 3 1
echo =2
fc -re cat 2 4
