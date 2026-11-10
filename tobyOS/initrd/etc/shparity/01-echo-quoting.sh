# Quoting is the surface every shell script touches first, and the one where
# a near-miss is hardest to spot: the output LOOKS right until a path with a
# space or a literal $ goes through it.
echo hello world
echo "double  spaced"
echo 'single $notexpanded'
echo "adjacent"'concat'
echo a"b"c'd'e
x=val
echo "$x" '$x' "\$x"
echo "quoted   spaces preserved"
echo unquoted   spaces   collapsed
echo "trailing backslash in quotes: \\"
echo ""
echo end
