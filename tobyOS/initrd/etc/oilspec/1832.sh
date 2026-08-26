# hm would it be saner to make FATAL builtins called break/continue/etc.?
# On the other hand, this spits out errors loudly.

echo '- break'
b=break
for i in 1 2 3; do
  echo $i
  $b
done

echo '- continue'
c='continue'
for i in 1 2 3; do
  if test $i = 2; then
    $c
  fi
  echo $i
done

r='return'
f() {
  echo '- return'
  for i in 1 2 3; do
    echo $i
    if test $i = 2; then
      $r 99
    fi
  done
}
f
echo status=$?

echo '- exit'
e='exit'
$e 5
echo 'not executed'
