# S6 history probe -- run under the GUEST's bash and under /bin/tsh:
#   bash hist-guest.sh 2>&1 | cat -A
# Every rule cmd_history implements is exercised here, one marker per block.
W=/tmp/hg.$$; mkdir -p $W; cd $W
echo "== A -s"
set -o history; history -c
echo one
history -s "pushed A"; history -s "pushed B"
history
history -c; history -s zz; echo "n=$(history | wc -l)"
history -s; echo "rc=$? n=$(history | wc -l)"
history -s a b  c; history 1
echo "== B -d"
history -c
history -s a1; history -s a2; history -s a3; history -s a4; history -s a5
history -d 2; echo "rc=$?"; history
history -d -1; echo "rc=$?"; history
history -d 99; echo "rc=$?"
history -d 0; echo "rc=$?"
history -d x; echo "rc=$?"
history -d -99; echo "rc=$?"
history -c; history -s b1; history -s b2; history -s b3; history -s b4; history -s b5; history -s b6
history -d 2-3; echo "rc=$?"; history
history -d -2--1; echo "rc=$?"; history
history -d 3-2; echo "rc=$?"; history | wc -l
history -d 0-1; echo "rc=$?"; history | wc -l
history -d 1-99; echo "rc=$?"; history | wc -l
history -d x-2; echo "rc=$?"
history -d; echo "rc=$?"
echo "== C -p"
history -c; echo p1
history -p 'x' y; echo "rc=$?"; history
history -p; echo "rc=$?"
echo "== D files"
history -c
echo f1
echo f2
HISTFILE=$W/hf
history -w; echo "rc=$? lines=$(wc -l < $W/hf)"
cat $W/hf
echo f3
history -a; echo "rc=$? lines=$(wc -l < $W/hf)"
history -a; echo "rc=$? lines=$(wc -l < $W/hf)"
history -c; history -r; echo "rc=$?"; history | wc -l
printf 'r1\n\nr2\n#1700000000\nr3\n' > $W/rf
history -c; history -r $W/rf; echo "rc=$?"; history
printf 'n1\nn2\n' > $W/nf; history -c; history -r $W/nf
printf 'n3\nn4\n' >> $W/nf; history -n $W/nf; echo "rc=$?"; history
history -w /nonexist/dir/f; echo "rc=$?"
history -r /nonexist/f; echo "rc=$?"
history -a /nonexist/dir/f; echo "rc=$?"
history -ar; echo "rc=$?"
history -cr $W/rf; echo "rc=$? n=$(history | wc -l)"
history -c; echo "rc=$?"
rm -f $W/new; history -s x; history -a $W/new; echo "rc=$? $(cat $W/new | wc -l)"
echo "== E HISTSIZE"
history -c
for i in 1 2 3 4 5 6 7 8 9; do history -s "s$i"; done
HISTSIZE=3
history
echo after; history
unset HISTSIZE; echo u1; echo u2; history | wc -l
HISTSIZE=-1; echo d; history | wc -l
HISTSIZE=0; history | wc -l; echo e; history | wc -l
HISTSIZE=abc; echo g; history | wc -l
HISTSIZE=100; history -c; echo z; history
echo "== F HISTFILESIZE"
printf 'c%s\n' 1 2 3 4 5 6 7 8 9 10 > $W/tf
HISTFILE=$W/tf
HISTFILESIZE=4; wc -l < $W/tf; cat $W/tf
HISTFILESIZE=-1; wc -l < $W/tf
HISTFILESIZE=9; wc -l < $W/tf
mkdir -p $W/home; printf 'h%s\n' 1 2 3 4 5 > $W/home/.history
unset HISTFILE; HOME=$W/home; HISTFILESIZE=2; wc -l < $W/home/.history
history -c; history -s fromhome; history -w; echo "rc=$?"; cat $W/home/.history
echo "== G listing args"
history -c; history -s l1; history -s l2; history -s l3
history abc; echo "rc=$?"
history 2 3; echo "same line"
echo "after discard rc=$?"
history -- 1; echo "rc=$?"
history +2; echo "rc=$?"
history -5; echo "rc=$?"
history -q; echo "rc=$?"
history 0; echo "rc=$?"
echo "== H reader"
history -c
  echo indented   # trailing comment
# a comment line

for i in 1 2; do
  echo "$i"
done
cat <<E
body
E
x=1 \
  y=2
printf 'echo sourced\n' > $W/s.sh; . $W/s.sh
eval 'echo ev'
echo "$(echo sub)"
history
echo "== I prompt"
PS1='foo \! bar'
history -c
echo "${PS1@P}"
echo "_${PS1@P}" | cat
echo "== J options"
set -o | grep -w history
set +o | grep -w history
[ -o history ]; echo "test=$?"
set +o history
set -o | grep -w history
history -s kept; history | tail -1
history -p q; history | tail -1
echo "== K off-reader"
history -c
echo notrec
history | wc -l
set -o history
cd /; rm -rf $W
echo done
