printf '%s\n' a{0..10} | { arr=(x y z); mapfile -O 2 -t arr; declare -p arr; }
printf '%s\n' a{0..10} | { mapfile -s 5 -n 3 -t arr; declare -p arr; }
printf '%s\n' a b c | { mapfile -n 0 -t q; declare -p q; }
mapfile -n x q < /dev/null; echo st=$?
help ZZZ; echo st=$?
help ec >/dev/null; echo st=$?
echo hi > a-{one,two}; echo st=$?
echo hi > b-{1..1}; cat b-1; rm -f b-1
2&>1; echo st=$?; rm -f 1
echo out &> o.txt; cat o.txt; rm -f o.txt
exec -a FOO sh -c 'echo $0'
