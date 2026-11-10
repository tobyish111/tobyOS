trap "echo int" INT
trap "echo e" EXIT
trap - int 0 3
trap

echo ---
trap "echo int" INT
trap "echo e" EXIT
trap - int 0 -99
if test $? -ne 0; then
  echo ok
fi
