exec 2>&1
echo -{z..A}-; echo "s=$?"
echo "s1=$?"
echo -{a..e}- -{e..a}- -{A..z..10}-
echo -{Z..a}-
echo "s2=$?"
echo -{A..z..30}-
[[ -f -f ]]; echo "status=$?"
[[ -f == ]]; echo "status=$?"
[[ -n -n ]]; echo "status=$?"
[[ -z == ]]; echo "status=$?"
s='abcd'; echo ${s: 0 < 1 ? 2 : 0 : 1} ${s:(1):2} ${s: 1 ? 1 : 0}
echo $"foo" $"foo $s" "$"x""
echo -n $'\c0\c9-' | od -An -tx1
echo -n $'\ca\cz\cA\cZ\c-\c+\c"\c?\c\' | od -An -tx1
