set -o errexit
shopt -s strict_errexit || true

if ! false; then
  echo yes
fi
