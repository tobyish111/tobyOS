filename=$TMP/lineno_regression3
rm -f $filename
echo x > $TMP/lineno_regression$LINENO
test -f $filename && echo written
echo $LINENO
