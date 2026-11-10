echo XX >| $TMP/c.txt

set -o noclobber

echo YY >  $TMP/c.txt  # not clobber
echo status=$?

cat $TMP/c.txt
echo ZZ >| $TMP/c.txt

cat $TMP/c.txt
