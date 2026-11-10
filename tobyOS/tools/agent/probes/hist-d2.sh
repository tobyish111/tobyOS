# history -d lower bounds, bash 5.2 (S6): numbers 1..5, then numbers 8..12
# (13 read, HISTSIZE=5 drops 8 and readline sets the base to 8).
set -o history
printf 'e%s\n' 1 2 3 4 5 > /tmp/hd5.$$
printf 'f%s\n' 1 2 3 4 5 6 7 8 9 10 11 12 13 > /tmp/hd13.$$
t()  { history -c; history -r /tmp/hd5.$$; }
t2() { history -c; history -r /tmp/hd13.$$; HISTSIZE=5; unset HISTSIZE; }
show() { history | sed 's/^ *//; s/  /:/' | tr '\n' ' '; }
for spec in 0-0 0-1 0-5 1-0 2-0 0-2 -5--1 -6--1 -6-2 0 1 -5; do
  t; history -d $spec; r=$?; echo "B1 [$spec] rc=$r left=$(show)"
done
t2; echo "B8 start: $(show)"
for spec in 0-8 1-8 7-8 7-9 8-8 8-12 12-12 12-13 -1--1 -5--1 -6--1 0-0 7 8 0; do
  t2; history -d $spec; r=$?; echo "B8 [$spec] rc=$r left=$(show)"
done
rm -f /tmp/hd5.$$ /tmp/hd13.$$
