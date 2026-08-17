# $((...)) arithmetic, including operator precedence and the comparison
# operators that return 1/0 rather than a shell exit status -- a distinction
# that is easy to get backwards.
echo $((1 + 2 * 3))
echo $(((1 + 2) * 3))
echo $((10 / 3))
echo $((10 % 3))
echo $((2 ** 8))
echo $((7 - 9))
n=5
echo $((n + 1))
echo $((n > 3))
echo $((n == 5))
echo $((n != 5))
i=0
i=$((i + 1))
i=$((i * 10))
echo $i
echo $(( 1 < 2 ? 100 : 200 ))
