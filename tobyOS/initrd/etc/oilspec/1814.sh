for i in a b c; do
  echo $i
  if test $i = b; then
    break
  fi
done
