# Associative keys and arithmetic subscripts as bash expands them (once, at
# assignment), the empty subscript's errors, and the text a bad substitution
# names -- the expansion unit, as bash reports it.
# (batch 12; probe p15 in C:\t\b12)
exec 2>&1
a=(1)
echo "1 ${a[]} tail"
echo pre${a[]}post
echo "x" "2 ${a[]} t" more
echo 'q'"3 ${a[]}"z
echo "4 $(echo in) ${a[]} ${a[0]}"
echo "5 ${a[0][0]} t"
echo 6${x:-"a ${a[]} b"}
echo "7 ${x:-a ${a[]} b} c"
echo "8 ${x/a/${a[]}} c"
cat <<E
9 ${a[]} here
E
echo "10 ${!x@} ${a[0]} ${b%%%} ${c:?} t"
echo "11 ${a[@]:0:-1} t"
echo "12 ${x:1 2} t"
