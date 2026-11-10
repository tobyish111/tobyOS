rm -f $TMP/no-clobber

set -C
echo foo >> $TMP/no-clobber
echo status=$?

cat $TMP/no-clobber
