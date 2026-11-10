trap 'echo line=$LINENO' ERR

false
echo a

set -o errexit

echo b
if false; then
  echo xx
fi
echo c  # doesn't get here
