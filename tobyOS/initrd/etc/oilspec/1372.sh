rm -f _tmp/r.txt
for x in a b c; do
  break > _tmp/r.txt
done
if test -f _tmp/r.txt; then
  echo REDIRECTED
else
  echo NO
fi
