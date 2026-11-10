# history -d range rules, bash 5.2 (S6). Each case starts from five entries
# e1..e5 (numbers 1..5, or shifted by a stifle in the last block); the -d line
# itself is entry 6 when it runs.
set -o history
printf 'e%s
' 1 2 3 4 5 > /tmp/hd5.$$
# -r, not -s: a `history -s` after `history -c` on the same line stores nothing.
t() { history -c; history -r /tmp/hd5.$$; }
for spec in 3-2 0-1 0-0 1-3 2-6 2-7 -1--2 -3--1 2--1 -99--1 -2-3 1-1 6-6 0 -6 -7 1--99; do
  t
  history -d $spec; r=$?
  echo "[$spec] rc=$r left=$(history | sed 's/^ *[0-9]*  //' | tr '\n' ' ')"
done
echo "-- shifted numbers (HISTSIZE=4 drops entries, the base moves)"
history -c
for i in 1 2 3 4 5 6 7 8; do history -s "s$i"; done
HISTSIZE=4
history
history -d 1; echo "rc=$?"
history -d 5; echo "rc=$?"; history
history -d 5-6; echo "rc=$?"; history
unset HISTSIZE
rm -f /tmp/hd5.$$
