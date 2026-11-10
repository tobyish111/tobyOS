# It shows the trap even though it doesn't execute it!

trap 'echo exit' EXIT

trap -p | cat > child.txt

grep EXIT child.txt >/dev/null
if test $? -eq 0; then
  echo shown
else
  echo not shown
fi
