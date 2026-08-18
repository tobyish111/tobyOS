[[ $(stdout_stderr.py) == STDOUT ]] 2>$TMP/x.txt
echo $?
echo --
cat $TMP/x.txt
