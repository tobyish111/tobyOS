set -o errexit
s=$(echo one; false)
echo status=$?
