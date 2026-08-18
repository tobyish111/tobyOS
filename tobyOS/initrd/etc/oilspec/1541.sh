set -o errexit
for x in 1 2 3; do
  test $x = 2 && echo "hi $x"
done
