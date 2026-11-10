debuglog() {
  echo "  [$@]"
}

trap 'debuglog $LINENO; return 42' DEBUG

echo status=$?
echo A
echo status=$?
echo B
echo status=$?



# OSH doesn't ignore this
