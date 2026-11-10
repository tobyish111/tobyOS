# The DEBUG trap runs before the command's words are expanded, so a trap
# that skips the command under extdebug skips its $( ) too -- tsh ran the
# $( ) first (finding 539).
exec 2>&1
trap 'echo "D $BASH_COMMAND"' DEBUG
echo $(echo X >&2)
x=$(echo Y >&2)
trap - DEBUG
shopt -s extdebug
trap '[[ $BASH_COMMAND != y=* ]]' DEBUG
y=$(echo SIDE >&2)
trap - DEBUG
echo "y=${y-unset}"
