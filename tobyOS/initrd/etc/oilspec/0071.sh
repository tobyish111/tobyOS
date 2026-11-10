$SH -c '
for i in foo; do
  continue 1 extra
done
echo status=$?
'
if test $? -eq 0; then
  echo fail
fi



status=1
fail
