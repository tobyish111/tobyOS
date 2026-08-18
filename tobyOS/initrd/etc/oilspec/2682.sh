tab=$(python2 -c 'print "\t42\t"')
cr=$(python2 -c 'print "\r42\r"')

$SH -c 'echo $(( $1 + 1 ))' dummy0 "$tab"
if test $? -ne 0; then
  echo 'failed'
fi

$SH -c 'echo $(( $1 + 1 ))' dummy0 "$cr"
if test $? -ne 0; then
  echo 'failed'
fi
