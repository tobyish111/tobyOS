echo 'echo rc' > rc

$SH --rcfile rc -i </dev/null 2>interactive.txt
grep -q 'warning' interactive.txt
echo warned=$? >&2

$SH --rcfile rc </dev/null 2>non-interactive.txt
grep -q 'warning' non-interactive.txt
echo warned=$? >&2

head *interactive.txt
