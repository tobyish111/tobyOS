debuglog() {
  echo "  [$@]"
}

trap 'debuglog $LINENO; exit 42' DEBUG

echo status=$?
echo A
echo status=$?
echo B
echo status=$?
