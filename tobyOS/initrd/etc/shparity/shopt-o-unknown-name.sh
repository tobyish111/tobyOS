# shopt -o of a name set -o does not have is "invalid option name", set's
# words, even under -q; asking is status 1, setting or unsetting 0. shopt's
# own unknown names keep "invalid shell option name" (finding 432).
exec 2>&1
shopt -p xyz1; echo "rc=$?"; shopt -o -p xyz1; echo "rc=$?"; shopt -q -o xyz2; echo "rc=$?"
shopt -s -o xyz3; echo "rc=$?"; shopt -u -o xyz4; echo "rc=$?"; shopt -q xyz5; echo "rc=$?"
