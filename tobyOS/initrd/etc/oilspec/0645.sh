help ZZZ 2>$TMP/err.txt
echo "help=$?"
cat $TMP/err.txt | grep -i 'no help topics' >/dev/null
echo "grep=$?"
