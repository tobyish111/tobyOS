# `>>` IS O_APPEND, AND EVERY WRITE GOES TO THE END. A group's `>> f` is
# one descriptor shared by the shell's own writes and every child's;
# /bin/tsh counted an offset of its own per copy and seeked each child to
# it, so the second child landed on the shell's last line ("one xx txx
# three"). And the kernel honoured O_APPEND only at open: two descriptions
# of one file -- `{ ...; echo two >> f; ...; } >> f`, `exec 4>> f` beside
# `echo q >> f` -- overwrote each other's lines.
exec 2>&1
d=/tmp/sp184; rm -rf $d; mkdir -p $d
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
# the end is where the file ends NOW: a truncation by someone else counts
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
printf 'l1\nl2\nl3\n' > $d/in
{ read a; echo "read=$a"; cat; } < $d/in
rm -rf $d
