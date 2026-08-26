set -o errexit
# false failure is NOT respected either way
{ echo foo; false; echo bar; } || echo "failed"

shopt -s strict_errexit || true
{ echo foo; false; echo bar; } || echo "failed"
