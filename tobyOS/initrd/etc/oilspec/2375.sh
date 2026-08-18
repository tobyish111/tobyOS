$SH -c '
for i
in one two three
do echo $i;
done
'
echo $?

$SH -c 'for i; in one two three; do echo $i; done'
test $? -ne 0 && echo cannot-parse
