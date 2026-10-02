# mapfile -O ORIGIN (and keep what the array holds), -s COUNT (skip), -n
# COUNT (at most; 0 is all). tsh refused all three as unsupported.
exec 2>&1
printf '%s\n' a{0..10} | { arr=(x y z); mapfile -O 2 -t arr; declare -p arr; }
printf '%s\n' a{0..10} | { mapfile -s 5 -n 3 -t arr; declare -p arr; }
printf '%s\n' a b c | { mapfile -n 0 -t q; declare -p q; }
mapfile -n x q < /dev/null; echo st=$?
