# TODO: We should probably use dash behavior of a fatal error.
# Although set-o errexit handles this.  We don't want to break the invariant
# that a builtin like 'source' behaves like an external program.  An external
# program can't halt the shell!
echo 'echo >' > $TMP/syntax-error.sh
. $TMP/syntax-error.sh
echo status=$?
