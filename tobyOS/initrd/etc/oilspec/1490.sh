(( a = $(stdout_stderr.py 42) + 10 )) 2>$TMP/x.txt
echo $a
echo --
cat $TMP/x.txt
