# A program's here-document is expanded in its child, so `$((n+=1))` in it
# does not reach the shell; a builtin's, function's or group's does.
# (batch 12; probe p21 in C:\t\b12)
exec 2>&1
n=0; cat <<E
$((n+=1))
E
echo "1 n=$n"
read x <<E
$((n+=5))
E
echo "2 n=$n x=$x"
f() { cat; }; f <<E
$((n+=10))
E
echo "3 n=$n"
{ cat; } <<E
$((n+=100))
E
echo "4 n=$n"
cat <<E | cat
$((n+=1000))
E
echo "5 n=$n"
