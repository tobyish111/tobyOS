arith=$(python2 -c 'print "argv.py $(( 1 +\n2))"')
arith_cr=$(python2 -c 'print "argv.py $(( 1 +\r\n2))"')

$SH -c "$arith"
if test $? -ne 0; then
  echo 'failed'
fi

$SH -c "$arith_cr"
if test $? -ne 0; then
  echo 'failed'
fi
