declare -a a=(x y)
argv.py "${!a[@]}"
echo a_keys=$?

argv.py "${!a}"  # missing [] is equivalent to ${!a[0]} ?
echo a_nobrackets=$?

echo ---
declare -A A=([A]=a [B]=b)

argv.py $(printf '%s\n' ${!A[@]} | sort)
echo A_keys=$?

(argv.py "${!A}")  # missing [] is equivalent to ${!A[0]} ?
echo A_nobrackets=$?
