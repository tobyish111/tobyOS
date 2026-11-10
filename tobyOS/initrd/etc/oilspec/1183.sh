debuglog() {
  echo "  [$@]"
}
trap 'debuglog $LINENO' DEBUG

while true; do
  echo hello
  break
done
