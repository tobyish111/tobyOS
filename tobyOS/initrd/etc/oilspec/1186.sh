# TODO: bash runs the trap 3 times, and osh only twice.  I don't see why.  Is
# it because Process::Run() does trap_state.ClearForSubProgram()?  Probably
#echo top PID=$$ BASHPID=$BASHPID
#shopt -s lastpipe

debuglog() {
  #echo "  PID=$$ BASHPID=$BASHPID LINENO=$1"
  #echo "  LINENO=$1 $BASH_COMMAND"
  # LINENO=6 echo pipeline
  # LINENO=7 cat
  echo "  LINENO=$1"
}
trap 'debuglog $LINENO' DEBUG

echo pipeline \
  | cat
echo ok
