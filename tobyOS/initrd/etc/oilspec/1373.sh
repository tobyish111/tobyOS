shopt -s ysh:all
rm -f _tmp/r.txt
for x in a b c; do
  break > _tmp/r.txt
done
test -f _tmp/r.txt && echo REDIRECTED
