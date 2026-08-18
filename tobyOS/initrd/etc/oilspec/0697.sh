touch -d 2017/12/31 $TMP/x
touch -d 2018/01/01 > $TMP/y
test $TMP/x -ot $TMP/y && echo 'older'
test $TMP/x -nt $TMP/y || echo 'not newer'
test $TMP/x -ot $TMP/x || echo 'not older than itself'
test $TMP/x -nt $TMP/x || echo 'not newer than itself'
