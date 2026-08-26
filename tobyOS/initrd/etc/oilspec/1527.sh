set -o errexit
shopt -s inherit_errexit || true
if echo $(echo 1; false; echo 2); then
  echo A
fi
echo done
