debuglog() {
  echo "  [$@]"
  return 42     # IGNORED FAILURE
}

trap 'debuglog $LINENO' DEBUG

echo status=$?
echo A
echo status=$?
echo B
echo status=$?
