set -o errexit
( echo one; false; echo two; )
echo three
