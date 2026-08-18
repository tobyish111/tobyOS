debuglog() {
  #echo "  PID=$$ BASHPID=$BASHPID LINENO=$1"
  echo "  LINENO=$1"
}
trap 'debuglog $LINENO' DEBUG

{ echo pipe1;
  echo pipe2; } \
  | cat
echo ok
