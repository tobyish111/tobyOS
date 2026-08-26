set -o errexit
readonly foo=bar
foo=eggs
echo "status=$?"  # nothing happens
