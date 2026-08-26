python2 -c 'print("echo -n %s" % ("x" * 65535))' > tmp.sh
$SH tmp.sh > out
wc --bytes out
