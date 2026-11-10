# A group's `>> FILE` is ONE descriptor opened O_APPEND: every write --
# a builtin's and a child's -- lands at the end, in order. Found when a
# probe's report came back interleaved (logs/b10/arith-before.txt).
d=/tmp/grpapp; rm -rf $d; mkdir -p $d
printf 'xx\n' > $d/f
{ echo one; cat $d/f; echo two; cat $d/f; echo three; } >> $d/ap
cat $d/ap
echo ---
: > $d/ap2
for i in 1 2 3; do { echo "head $i"; cat $d/f; echo "tail $i"; } >> $d/ap2; done
cat $d/ap2
echo ---
{ echo a; cat $d/f; echo b; } > $d/tr
cat $d/tr
echo ---
exec 3>> $d/ap3
echo x >&3; cat $d/f >&3; echo y >&3
exec 3>&-
cat $d/ap3
echo ---
# two DESCRIPTIONS of one file, both O_APPEND: each write goes to the end
: > $d/ap4
{ echo one; echo two >> $d/ap4; echo three; } >> $d/ap4
cat $d/ap4
echo ---
: > $d/ap5
exec 4>> $d/ap5
echo p >&4; echo q >> $d/ap5; echo r >&4; printf 's\n' >> $d/ap5; echo t >&4
exec 4>&-
cat $d/ap5
echo ---
# an append descriptor follows a truncation by someone else
exec 5>> $d/ap6
echo before >&5; : > $d/ap6; echo after >&5
exec 5>&-
od -c $d/ap6
echo ---
# the descriptor says what it is: write-only, append
exec 6>> $d/ap7
grep flags /proc/self/fdinfo/6
exec 6>&-
echo ---
# input: a builtin reads a line, the child gets the rest
printf 'l1\nl2\nl3\n' > $d/in
{ read a; echo "read=$a"; cat; } < $d/in
rm -rf $d
