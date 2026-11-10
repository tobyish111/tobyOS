echo '- break'
for i in 1 2 3; do
  echo $i
  \break
done

echo '- continue'
for i in 1 2 3; do
  if test $i = 2; then
    \continue
  fi
  echo $i
done

f() {
  echo '- return'
  for i in 1 2 3; do
    echo $i
    if test $i = 2; then
      \return 99
    fi
  done
}
f
echo status=$?

echo '- exit'
\exit 5
echo 'not executed'
