case $SH in dash) echo 'flaky'; exit ;; esac

set -C

rm -f $TMP/no-clobber
echo foo >> $TMP/no-clobber
echo stdout=$?
echo bar >> $TMP/no-clobber
echo again=$?
cat $TMP/no-clobber

rm -f $TMP/no-clobber
echo baz &>> $TMP/no-clobber
echo both=$?
echo foo &>> $TMP/no-clobber
echo again=$?
cat $TMP/no-clobber
