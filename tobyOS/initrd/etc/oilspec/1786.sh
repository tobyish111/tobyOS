# it ends with _history
$SH --norc -i -c 'echo HISTFILE=$HISTFILE' | egrep -q '_history$'
echo status=$?
