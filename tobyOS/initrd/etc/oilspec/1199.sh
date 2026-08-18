trap 'echo line=$LINENO' ERR

false
echo a

set -o errexit

echo b
false   # trap executed, and executation also halts
echo c  # doesn't get here
