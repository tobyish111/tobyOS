# An array literal may span lines, and may contain comments and blanks.
a=(
  one
  two

  three
)
echo "n=${#a[@]} all=${a[*]}"
b=(alpha
beta)
echo "b n=${#b[@]} 1=${b[1]}"
echo end
