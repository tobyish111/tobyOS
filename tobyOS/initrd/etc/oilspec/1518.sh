set -o errexit
fun() { echo fun; }

fun || true  # this is OK

shopt -s strict_errexit || true

echo 'builtin ok' || true
env echo 'external ok' || true

fun || true  # this fails
