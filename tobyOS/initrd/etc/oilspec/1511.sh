set -o errexit
echo $(echo one; false)  # we lost the exit code
echo status=$?
