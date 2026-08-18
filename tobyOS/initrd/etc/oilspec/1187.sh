debuglog() {
  echo "  [$@]"
}
trap 'debuglog $LINENO' DEBUG

# gets run for each one of these
{ echo a; echo b; }

# only run for the last one, maybe I guess because traps aren't inherited?
{ echo x; echo y; } | wc -l

# bash runs for all of these, but OSH doesn't because we have SubProgramThunk
# Hm.
date | cat | wc -l

date |
  cat |
  wc -l


# Marking OK due to lastpipe execution difference
