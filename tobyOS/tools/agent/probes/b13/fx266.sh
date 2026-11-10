d=/tmp/fxd.$$; rm -rf $d; mkdir $d; cd $d
# `enable` AS bash's (batch 13): the listing in bash's order, -s for the
# special builtins, -f answered as the guest's static bash answers it, -d
# "not dynamically loaded", and the usage line after a bad option. tsh
# listed its own table order and called -s, -f and -d invalid.
# (logs/b13 q39 q40)
exec 2>&1
enable -a | grep -v -e getconf -e logname -e pathchk | head -12
enable -s
enable -n echo test; enable -n; enable -p | grep -- -n
enable echo test
enable -f /x.so foo; echo "st=$?"
enable -d foo; echo "st=$?"
enable -d echo; echo "st=$?"
enable nosuch; echo "st=$?"
enable -x; echo "st=$?"
echo end
