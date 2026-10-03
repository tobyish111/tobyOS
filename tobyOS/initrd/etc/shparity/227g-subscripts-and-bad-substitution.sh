# Associative keys and arithmetic subscripts as bash expands them (once, at
# assignment), the empty subscript's errors, and the text a bad substitution
# names -- the expansion unit, as bash reports it.
# (batch 12; probe p16 in C:\t\b12)
exec 2>&1
a=(1)
x=${a[]}; echo s1
export x2=${a[]}; echo s2
declare y=pre${a[]}; echo s3
a[1]=${a[]}; echo s4
x+=${a[]}; echo s5
x3=q${a[]} env true; echo s6
echo "8 ${u/a/${a[]}} c"
e=; echo "9 ${e/a/${a[]}} c"
v=abc; echo "10 ${v/zz/${a[]}} c"
echo "11 ${u/${a[]}/r} c"
echo "12 ${u#${a[]}} c"
echo "13 ${u:-${a[]}} c"
echo "14 ${v:+${a[]}} c"
cat <<E
15 $LINENO
E
cat <<E
16 ${a[]} $LINENO
E
echo "17 $LINENO"
