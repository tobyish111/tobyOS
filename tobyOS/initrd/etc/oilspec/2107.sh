exec {myfd}> $TMP/named-fd.txt
echo named-fd-contents >& $myfd
cat $TMP/named-fd.txt
