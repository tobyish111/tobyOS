set -o errexit
shopt -s strict:all || true
echo one
false  # fail
echo two
