# Associative keys and arithmetic subscripts as bash expands them (once, at
# assignment), the empty subscript's errors, and the text a bad substitution
# names -- the expansion unit, as bash reports it.
# (batch 12; probe p14 in C:\t\b12)
exec 2>&1
a=(1 2); echo "1 ${a[]}"; echo s=$?
echo "2 ${a[]:-x}"
echo 3 ${a[]}
x=${a[]}; echo "4 [$x] $?"
echo "5 ${#a[]}"
echo "6 ${!a[]}"
echo "7 ${a[]#x}"
