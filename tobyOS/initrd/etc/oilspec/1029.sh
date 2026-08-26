# I didn't know these existed -- I only knew about - and 0 !
# Note: '#' flag for integers outputs a prefix ONLY WHEN the value is non-zero
printf '[%#o][%#o]\n' 0 42
printf '[%#x][%#x]\n' 0 42
printf '[%#X][%#X]\n' 0 42
echo ---
# Note: '#' flag for %f, %g always outputs the decimal point.
printf '[%.0f][%#.0f]\n' 3 3
# Note: In addition, '#' flag for %g does not omit zeroes in fraction
printf '[%g][%#g]\n' 3 3
