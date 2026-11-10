for i in 1 2 3; do
  echo i=$i
  while break; do
    echo x
  done
done
echo done
