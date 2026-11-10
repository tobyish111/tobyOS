$SH -c '
(( echo 1
echo 2
(( x ))
: $(( x ))
echo 3
))
'
if test $? -ne 0; then
  echo ok
fi
