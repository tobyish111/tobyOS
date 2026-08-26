python2 -c 'print("echo -n %s" % ("x" * 65536))' > tmp.sh
$SH tmp.sh > out
echo status=$?
wc --bytes out
