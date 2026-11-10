# bash/mksh treat this like a filename, not a descriptor.
# dash aborts.
echo one 1>&$TMP/nonexistent-filename__
echo "status=$?"
