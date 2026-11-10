set -o errexit
# It is respected here.
{ echo one; false; echo two; } | cat

# Also respected here.
{ echo three; echo four; } | while read line; do
  echo "[$line]"
  false
done
echo four
