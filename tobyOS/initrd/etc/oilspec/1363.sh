x=foo
result='-'
case "$x" in
  f*|*o) result="$result X"
esac
echo $result
