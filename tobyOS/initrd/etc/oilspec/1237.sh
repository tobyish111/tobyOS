# All shells spew warnings to stderr, but don't actually exit!  Bad!
trap 'echo <' EXIT
echo status=$?
