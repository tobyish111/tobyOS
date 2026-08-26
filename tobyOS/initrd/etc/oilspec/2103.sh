# This time 1 *is* a descriptor, not a word.  If you add a space between 1 and
# >, it doesn't work.
echo two 1> $TMP/file-redir1.txt
cat $TMP/file-redir1.txt
