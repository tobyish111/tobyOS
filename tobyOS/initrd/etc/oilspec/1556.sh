set -o errexit
{ echo one; false; echo two; exit 42; } &
wait $!
