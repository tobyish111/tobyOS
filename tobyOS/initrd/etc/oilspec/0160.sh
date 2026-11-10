echo $(( 5 << 1 ))
echo $(( 5 << 0 ))
$SH -c 'echo $(( 5 << -1 ))'  # implementation defined - OSH fails
echo ---

echo $(( 16 >> 1 ))
echo $(( 16 >> 0 ))
$SH -c 'echo $(( 16 >> -1 ))'  # not sure why this is zero
$SH -c 'echo $(( 16 >> -2 ))'  # also 0
echo ---
