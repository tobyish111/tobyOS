cd $TMP
echo 'foo >' > bad_oshrc

$SH --rcfile bad_oshrc -i -c 'echo hi' 2>stderr.txt
echo status=$?

# bash prints two lines
grep --max-count 1 -o 'bad_oshrc:' stderr.txt
