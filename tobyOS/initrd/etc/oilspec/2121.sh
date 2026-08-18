# dash, mksh don't implement this bash behaviour.
case $SH in dash|mksh) exit 1 ;; esac

tmp="$(basename $SH)-$$.txt"  # unique name for shell and test case

stdout_stderr.py >&$tmp

# order is indeterminate
grep STDOUT $tmp
grep STDERR $tmp
