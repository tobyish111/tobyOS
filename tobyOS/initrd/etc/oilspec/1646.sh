iters=0

for ((i = 1 << 32; i; ++i)); do
  echo $i
  iters=$(( iters + 1 ))
  if test $iters -eq 5; then
    break
  fi
done
