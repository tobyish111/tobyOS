case $SH in dash|zsh) exit ;; esac

echo '- break'
for i in 1 2 3; do
  echo $i
  builtin break
done

echo '- continue'
for i in 1 2 3; do
  if test $i = 2; then
    command continue
  fi
  echo $i
done

f() {
  echo '- return'
  for i in 1 2 3; do
    echo $i
    if test $i = 2; then
      builtin command return 99
    fi
  done
}
f
echo status=$?

echo '- exit'
command builtin exit 5
echo 'not executed'
