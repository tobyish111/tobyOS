set -o nounset
x=$(( y + 5 ))
echo "should not get here: x=${x:-<unset>}"
