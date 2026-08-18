debuglog() {
  echo "  [$@]"
}
trap 'debuglog $LINENO' DEBUG

f() {
  local mylocal=1
  for i in "$@"; do
    echo i=$i
  done
}

f A B  # executes ONCE here, but does NOT go into the function call

echo next

f X Y

echo ok
