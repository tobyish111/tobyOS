for i in a b c; do
  echo $i
  if test $i = b; then
    continue
  fi
  echo $i
done
