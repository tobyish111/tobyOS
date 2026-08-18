# Inner `` are escaped!  Not sure how to do triple..  Seems like an unlikely
# use case.  Not sure if I even want to support this!
echo X > $TMP/000000-first
echo `\`echo -n l; echo -n s\` $TMP | grep 000000-first`
