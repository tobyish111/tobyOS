# POSIX XCU read -- the counterpart to field splitting.
# The LAST variable absorbs everything left over, IFS controls the split, and
# -r turns off backslash processing. Reading a file line by line is the single
# most common shell idiom that depends on all three.
printf 'one two three\n' > r1.txt
read a b c < r1.txt
echo "1:[$a][$b][$c]"

printf 'one two three four five\n' > r2.txt
read a b c < r2.txt
echo "2:[$a][$b][$c]"

printf 'single\n' > r3.txt
read a b c < r3.txt
echo "3:[$a][$b][$c]"

printf '  padded   fields  \n' > r4.txt
read a b < r4.txt
echo "4:[$a][$b]"

printf 'x:y:z\n' > r5.txt
IFS=: read a b c < r5.txt
echo "5:[$a][$b][$c]"
echo "6:ifs-restored=[$IFS]"

printf 'a\\tb\n' > r6.txt
read -r line < r6.txt
echo "7:[$line]"

printf 'l1\nl2\nl3\n' > r7.txt
n=0
while read line; do n=$((n+1)); echo "8:$n:$line"; done < r7.txt
echo "9=$n"

printf 'no-newline-at-end' > r8.txt
read line < r8.txt
echo "10:[$line] rc-nonzero=$?"

printf '\n' > r9.txt
read line < r9.txt
echo "11:[$line]"

printf 'k=v\n' > r10.txt
while IFS== read k v; do echo "12:[$k]=[$v]"; done < r10.txt
echo end
