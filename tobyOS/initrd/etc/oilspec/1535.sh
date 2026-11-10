set -o errexit
if { echo one; false; echo two; }; then
  echo three
fi
echo four
