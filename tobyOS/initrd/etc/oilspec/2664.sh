# hm bash doesn't take into account the word break.  That's OK; we won't either.
echo one
for x in \
  $LINENO zzz; do
  echo $x
done
