{ echo foo52 1>&2; echo 012345789; } > $TMP/block-stdout.txt
cat $TMP/block-stdout.txt |  wc -c 
