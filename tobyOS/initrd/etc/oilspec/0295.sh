# http://lists.gnu.org/archive/html/help-bash/2017-09/msg00005.html

set -o nounset
empty=()
argv.py "${empty[@]}"
echo status=$?
