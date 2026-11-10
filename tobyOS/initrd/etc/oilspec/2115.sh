f=''
echo s > "$f"
echo "result=$?"
set -o errexit
echo s > "$f"
echo DONE
