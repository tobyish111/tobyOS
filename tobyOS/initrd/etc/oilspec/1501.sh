set -o errexit
shopt -s strict_errexit || true

if echo 1 | grep 1; then
  echo one
fi
