debuglog() {
  echo "  [$@]"
}
trap 'debuglog $LINENO' DEBUG

if test x = x; then
  echo if
fi 

while test x != x; do
  echo while
done
