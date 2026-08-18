set -o errexit
s=$(echo one; false; echo two;)
echo "$s"
# dash and mksh: whole thing aborts!
