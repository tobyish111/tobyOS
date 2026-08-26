# NOTE: osh failing because of file descriptor issue.  stdin has to be closed!
tmp=$TMP/$(basename $SH)-readr.txt
echo -e 'one\\\ntwo\n' > $tmp
read escaped < $tmp
read -r raw < $tmp
argv.py "$escaped" "$raw"
